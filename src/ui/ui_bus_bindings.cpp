// ui_bus_bindings.cpp — EEZ akce → LIN zápis + sync model → UI
#include "ui_bus_bindings.h"
#include "ui_display_mgr.h"

#include "app_cmd.h"
#include "src/bus_lg_lin_api.h"
#include "src/bus_lg_model.h"
#include "src/bus_lg_protocol.h"
#include "climate_plan.h"
#include "src/climate_scheduler.h"
#include "climate_regulator.h"
#include "climate_energy.h"
#include "storage_config_nvs.h"
#include "src/ui_eez_model.h"

#include <Arduino.h>
#include <esp_log.h>

namespace {

static const char* TAG = "UI_BUS";

constexpr uint8_t kWaterMinC = REG_T_WATER_MIN_C;
constexpr uint8_t kWaterMaxC = REG_T_WATER_MAX_C;

const char* spSrcName(UiSpSource src) {
  switch (src) {
    case UI_SP_SRC_HMI:
      return "HMI";
    case UI_SP_SRC_MQTT:
      return "MQTT";
    case UI_SP_SRC_REGULATOR:
      return "REG";
    case UI_SP_SRC_PLAN:
      return "PLAN";
    default:
      return "?";
  }
}

/** Varianta A: Auto → jen regulátor; ruční → HMI / MQTT / plán. */
bool allowWaterSpWrite(UiSpSource src) {
  if (uiEez.rezim == UI_REZIM_AUTO) {
    return src == UI_SP_SRC_REGULATOR;
  }
  return src == UI_SP_SRC_HMI || src == UI_SP_SRC_MQTT || src == UI_SP_SRC_PLAN;
}

/** Pokojový SP: HMI nebo MQTT (last-write-wins). */
bool allowRoomSpWrite(UiSpSource src) {
  return src == UI_SP_SRC_HMI || src == UI_SP_SRC_MQTT;
}

uint8_t clampWaterC(int t) {
  if (t < (int)kWaterMinC) {
    return kWaterMinC;
  }
  if (t > (int)kWaterMaxC) {
    return kWaterMaxC;
  }
  return (uint8_t)t;
}

uint8_t aktualniCilovaTeplota() {
  if (pozadavekNaZapis) {
    return clampWaterC(novaCilovaTeplota);
  }
  if (mCilova >= kWaterMinC && mCilova <= kWaterMaxC) {
    return mCilova;
  }
  lgModelLock();
  const uint8_t a0Sp = lgMaCerstoA0() ? lgModelA0Bajt(8) : 0;
  lgModelUnlock();
  if (a0Sp >= kWaterMinC && a0Sp <= kWaterMaxC) {
    return a0Sp;
  }
  return clampWaterC(35);
}

bool drzenyZapnuty() {
  return cilovyZapnutoTab5 || tcPozadavekZap || cekameNaOrigStart;
}

/** Plán VYP: TČ vypnuto na sběrnici, ale uživatelský START zůstává (resume po VYP). */
bool s_planSessionHold = false;

void provedStop() {
  if (lgZapisBezi()) {
    ESP_LOGW(TAG, "STOP odlozen — probiha LIN sekvence");
  }

  lgModelLock();
  const uint8_t t = aktualniCilovaTeplota();
  lgModelUnlock();

  if (cekameNaOrigStart) {
    lgUkonciCekaniProStop();
  }
  s_planSessionHold = false;
  tcPozadavekZap = false;
  stavZapnuto = false;
  lgNastavDrzenyStav(t, false);

  lgModelLock();
  novaCilovaTeplota = t;
  pozadavekZmenaStartu = true;
  pozadavekNaZapis = true;
  lgModelUnlock();

  uiEez.sig_chod = false;
  uiEez.stav_tc = UI_STAV_VYP;
  ESP_LOGI(TAG, "STOP T=%u (session OFF)", (unsigned)t);
}

/** Dočasné vypnutí z plánu — LIN STOP, session START se nemaže. */
void provedPlanSuspend() {
  if (lgZapisBezi()) {
    ESP_LOGW(TAG, "PLAN VYP odlozen — probiha LIN sekvence");
  }

  lgModelLock();
  const uint8_t t = aktualniCilovaTeplota();
  lgModelUnlock();

  const bool wasSession = drzenyZapnuty() || s_planSessionHold;

  if (cekameNaOrigStart) {
    lgUkonciCekaniProStop();
  }
  tcPozadavekZap = false;
  stavZapnuto = false;

  // Držet VYP na sběrnici, ale NVS/session nechat jako START (pokud byl).
  cilovaTeplotaTab5 = t;
  cilovyZapnutoTab5 = false;
  drzetStavAktivni = true;
  if (wasSession) {
    s_planSessionHold = true;
    storageRequestSaveTcSession(true, t);
  } else {
    s_planSessionHold = false;
    storageRequestSaveTcSession(false, t);
  }

  lgModelLock();
  novaCilovaTeplota = t;
  pozadavekZmenaStartu = true;
  pozadavekNaZapis = true;
  lgModelUnlock();

  // Fyzicky neběží (CHOD off), session hold jen pro logiku plánu / tlačítka.
  uiEez.sig_chod = false;
  uiEez.stav_tc = UI_STAV_VYP;
  ESP_LOGI(TAG, "PLAN VYP T=%u (session %s)", (unsigned)t,
           wasSession ? "HOLD/START" : "OFF");
}

void provedStart() {
  lgModelLock();
  const uint8_t b2 = lgModelA0Bajt(2);
  const uint8_t b3 = lgModelA0Bajt(3);
  const bool uzDrzeny = drzenyZapnuty();
  // Jen skutečný topný běh (ZAP / 0x0A) — NE B3=0x08 útlum po SP (to by vrátilo STOP).
  const bool tcTopi = lgJeZapnuto(b3) || lgJeStabilniBeh(b3);
  uint8_t t = aktualniCilovaTeplota();
  lgModelUnlock();

  s_planSessionHold = false;

  // Auto: START s cílem regulátoru, ne se starým ručním SP z HMI/A0
  if (uiEez.rezim == UI_REZIM_AUTO) {
    RegulatorSnapshot snap{};
    climateRegulatorGetSnapshot(&snap);
    t = snap.t_water_c;
  }

  if (uzDrzeny && cilovyZapnutoTab5) {
    ESP_LOGI(TAG, "START ignorovan — uz zapnuto");
    return;
  }

  if (tcTopi) {
    lgModelLock();
    novaCilovaTeplota = t;
    mCilova = t;
    tcPozadavekZap = true;
    lgNastavDrzenyStav(t, true);
    // Adopt bez C0 START — SP z regulátoru stejně poslat (A0 může mít starý cíl)
    const uint8_t a0Sp = lgModelA0Bajt(8);
    if (a0Sp != t) {
      pozadavekZmenaStartu = false;
      pozadavekNaZapis = true;
    }
    lgModelUnlock();
    uiEez.sig_chod = true;
    uiEez.stav_tc = UI_STAV_BEH;
    uiEez.sp_pending = t;
    uiEez.sp_pending_ms = millis();
    if (uiEez.rezim == UI_REZIM_AUTO) {
      climateRegulatorRequestImmediateTick();
    }
    ESP_LOGI(TAG, "START adopt (T/C uz topi) T=%u", (unsigned)t);
    return;
  }

  lgModelLock();
  novaCilovaTeplota = t;
  mCilova = t;
  tcPozadavekZap = true;
  lgNastavDrzenyStav(t, true);
  pozadavekZmenaStartu = true;
  pozadavekNaZapis = true;
  lgModelUnlock();

  uiEez.sig_chod = true;
  uiEez.stav_tc = UI_STAV_PRESTART;

  if (uiEez.rezim == UI_REZIM_AUTO) {
    climateRegulatorRequestImmediateTick();
  }

  ESP_LOGI(TAG, "START T=%u (session ON)%s", (unsigned)t,
           uiEez.rezim == UI_REZIM_AUTO ? " Auto" : "");
}

void provedStartStopToggle() {
  // Session HOLD (plán VYP) = pořád „START režim“ → tlačítko dělá plný STOP
  if (drzenyZapnuty() || s_planSessionHold) {
    provedStop();
  } else {
    provedStart();
  }
}

void provedTeplotaAbsolutni(uint8_t nova, UiSpSource src) {
  nova = clampWaterC(nova);
  if (!allowWaterSpWrite(src)) {
    ESP_LOGW(TAG, "SP vody %u blokovan (src=%s rezim=%s)", (unsigned)nova,
             spSrcName(src),
             uiEez.rezim == UI_REZIM_AUTO ? "AUTO" : "MAN");
    return;
  }

  // Ruční HMI/MQTT: fronta do linTask (bez race na lgZapis)
  if (uiEez.rezim != UI_REZIM_AUTO &&
      (src == UI_SP_SRC_HMI || src == UI_SP_SRC_MQTT)) {
    if (!drzenyZapnuty() && !s_planSessionHold && !stavZapnuto) {
      ESP_LOGW(TAG, "SP vody %u ignorovan — nejdriv START (src=%s)", (unsigned)nova,
               spSrcName(src));
      return;
    }

    lgModelLock();
    novaCilovaTeplota = nova;
    mCilova = nova;
    if (drzetStavAktivni) {
      cilovaTeplotaTab5 = nova;
    }
    pozadavekZmenaStartu = false;
    pozadavekNaZapis = true;
    lgModelUnlock();

    uiEez.teplota_vody_set = static_cast<float>(nova);
    uiEez.sp_pending = nova;
    uiEez.sp_pending_ms = millis();
    potrebaObnovitDisplej = true;

    storageRequestSaveTcSession(cilovyZapnutoTab5, nova);

    ESP_LOGI(TAG, "setpoint cmd -> %u C src=%s", (unsigned)nova, spSrcName(src));
    return;
  }

  const bool zap = drzenyZapnuty() || stavZapnuto ||
                   lgJeTcProvoz(lgModelA0Bajt(2), lgModelA0Bajt(3));

  lgModelLock();
  novaCilovaTeplota = nova;
  mCilova = nova;
  if (drzetStavAktivni || zap) {
    lgNastavDrzenyStav(nova, zap || cilovyZapnutoTab5);
  }
  lgModelUnlock();

  uiEez.teplota_vody_set = static_cast<float>(nova);
  uiEez.sp_pending = nova;
  uiEez.sp_pending_ms = millis();
  potrebaObnovitDisplej = true;

  ESP_LOGI(TAG, "setpoint cmd -> %u C src=%s zap=%d auto=%d", (unsigned)nova,
           spSrcName(src), (int)zap, (int)(uiEez.rezim == UI_REZIM_AUTO));

  // Auto + T/C neběží: jen cmd pro další START (bez LIN VYP paketu)
  if (uiEez.rezim == UI_REZIM_AUTO && !zap) {
    ESP_LOGI(TAG, "Auto SP %u C — bez LIN (T/C nebezi)", (unsigned)nova);
    return;
  }

  lgModelLock();
  pozadavekNaZapis = true;
  pozadavekZmenaStartu = false;
  lgModelUnlock();
}

void provedTeplotaZmena(int delta, UiSpSource src) {
  lgModelLock();
  const uint8_t aktualniCilova = aktualniCilovaTeplota();
  const int nova = (int)aktualniCilova + delta;
  lgModelUnlock();
  if (nova < (int)kWaterMinC || nova > (int)kWaterMaxC) {
    return;
  }
  provedTeplotaAbsolutni((uint8_t)nova, src);
}

void provedRoomSpZmena(float deltaC, UiSpSource src) {
  if (!allowRoomSpWrite(src)) {
    ESP_LOGW(TAG, "room SP adjust blokovan (src=%s)", spSrcName(src));
    return;
  }
  climateRegulatorAdjustRoomSp(deltaC);
  ESP_LOGI(TAG, "Auto room SP -> %.1f (src=%s)",
           (double)climateRegulatorGetConfig()->room_sp_c, spSrcName(src));
}

void provedRoomSpAbs(float c, UiSpSource src) {
  if (!allowRoomSpWrite(src)) {
    ESP_LOGW(TAG, "room SP abs blokovan (src=%s)", spSrcName(src));
    return;
  }
  climateRegulatorSetRoomSp(c);
  ESP_LOGI(TAG, "Auto room SP abs -> %.1f (src=%s)",
           (double)climateRegulatorGetConfig()->room_sp_c, spSrcName(src));
}

void processAppMsg(const AppMsg& msg) {
  const bool autoMode = (uiEez.rezim == UI_REZIM_AUTO);

  switch (msg.cmd) {
    case APP_CMD_HMI_ACTION:
      uiBusHandleAkce(static_cast<UiAkceTlacitko>(msg.arg));
      break;
    case APP_CMD_POWER_START:
      ESP_LOGI(TAG, "queue → START (src=%s)", spSrcName(msg.src));
      provedStart();
      break;
    case APP_CMD_POWER_STOP:
      ESP_LOGI(TAG, "queue → STOP (src=%s)", spSrcName(msg.src));
      provedStop();
      break;
    case APP_CMD_SETPOINT_ABS:
      if (autoMode) {
        provedRoomSpAbs((float)msg.arg / 10.0f, msg.src);
      } else {
        provedTeplotaAbsolutni((uint8_t)msg.arg, msg.src);
      }
      break;
    case APP_CMD_SETPOINT_DELTA:
      if (autoMode) {
        provedRoomSpZmena((float)msg.arg / 10.0f, msg.src);
      } else {
        provedTeplotaZmena(msg.arg, msg.src);
      }
      break;
    case APP_CMD_SET_MODE:
      uiBusSetRegulationAuto(msg.arg != 0);
      ESP_LOGI(TAG, "queue → mode %s (src=%s)",
               msg.arg ? "room" : "water", spSrcName(msg.src));
      break;
    default:
      break;
  }
}

}  // namespace

void uiBusHandleAkce(UiAkceTlacitko akce) {
  switch (akce) {
    case UI_AKCE_START:
      provedStart();
      break;
    case UI_AKCE_STOP:
      provedStop();
      break;
    case UI_AKCE_START_STOP:
      provedStartStopToggle();
      break;
    case UI_AKCE_TEPLOTA_PLUS:
      if (uiEez.rezim == UI_REZIM_AUTO) {
        provedRoomSpZmena(0.5f, UI_SP_SRC_HMI);
      } else {
        provedTeplotaZmena(1, UI_SP_SRC_HMI);
      }
      break;
    case UI_AKCE_TEPLOTA_MINUS:
      if (uiEez.rezim == UI_REZIM_AUTO) {
        provedRoomSpZmena(-0.5f, UI_SP_SRC_HMI);
      } else {
        provedTeplotaZmena(-1, UI_SP_SRC_HMI);
      }
      break;
    case UI_AKCE_REZIM_PREPNOUT:
      if (uiEez.rezim == UI_REZIM_AUTO) {
        uiEez.rezim = UI_REZIM_VYSTUPNI_TEPLOTA;
        // Session SP = aktuální A0
        lgModelLock();
        {
          const uint8_t a0Sp = lgModelA0Bajt(8);
          if (a0Sp >= 15 && a0Sp <= 65) {
            mCilova = a0Sp;
            if (drzetStavAktivni) {
              cilovaTeplotaTab5 = a0Sp;
            }
          }
        }
        lgModelUnlock();
      } else {
        uiEez.rezim = UI_REZIM_AUTO;
        if (drzenyZapnuty()) {
          RegulatorSnapshot snap{};
          climateRegulatorGetSnapshot(&snap);
          provedTeplotaAbsolutni(snap.t_water_c, UI_SP_SRC_REGULATOR);
        } else {
          ESP_LOGI(TAG, "Auto zapamatovan — ceka START (cerpadlo vyp)");
        }
      }
      uiBusPersistRezim();
      ESP_LOGI(TAG, "rezim -> %s",
               uiEez.rezim == UI_REZIM_AUTO ? "AUTO" : "VYSTUPNI");
      break;
    default:
      break;
  }
}

void uiBusSetWaterSp(uint8_t teplotaC, UiSpSource src) {
  provedTeplotaAbsolutni(teplotaC, src);
}

void uiBusSetSetpointC(uint8_t teplotaC) {
  provedTeplotaAbsolutni(teplotaC, UI_SP_SRC_REGULATOR);
}

void uiBusAdjustSetpoint(int deltaC) {
  if (uiEez.rezim == UI_REZIM_AUTO) {
    provedRoomSpZmena((float)deltaC / 10.0f, UI_SP_SRC_HMI);
  } else {
    provedTeplotaZmena(deltaC, UI_SP_SRC_HMI);
  }
}

void uiBusQueuePower(bool start) {
  appCmdEnqueuePower(start, UI_SP_SRC_MQTT);
}

void uiBusQueueSetpointC(uint8_t teplotaC) {
  appCmdEnqueueSetpointAbs((int)teplotaC, UI_SP_SRC_MQTT);
}

void uiBusQueueAdjustSetpoint(int deltaC) {
  appCmdEnqueueAdjust(deltaC, UI_SP_SRC_MQTT);
}

void uiBusQueueSetRegulationAuto(bool roomMode) {
  appCmdEnqueueMode(roomMode, UI_SP_SRC_MQTT);
}

void uiBusPlanApplyStart(void) {
  provedStart();
}

void uiBusPlanApplyStop(void) {
  // Ne jako tlačítko STOP — session START zůstává (resume po konci VYP)
  provedPlanSuspend();
}

void uiBusPlanApplySetpoint(uint8_t teplotaC) {
  // Auto: plán nesmí měnit SP vody (jen offset/VYP přes climate_plan)
  provedTeplotaAbsolutni(teplotaC, UI_SP_SRC_PLAN);
}

bool uiBusSessionIsOn(void) {
  return drzenyZapnuty() || s_planSessionHold;
}

bool uiBusSetRegulationAuto(bool enable) {
  uiEez.rezim = enable ? UI_REZIM_AUTO : UI_REZIM_VYSTUPNI_TEPLOTA;
  uiBusPersistRezim();
  return true;
}

void uiBusPersistRezim(void) {
  storageSaveUiRezim((uint8_t)uiEez.rezim);
}

void uiBusBindingsTick(void) {
  appCmdDrainCtrl();
  climateSchedulerTick();
  climatePlanTick();
  climateRegulatorTick();
  climateEnergyTick();
  uiEezSyncFromBus();
}

void uiBusFlushDeferredStorage(void) {
  static uint32_t s_lastFlushMs = 0;
  const uint32_t now = millis();
  if (s_lastFlushMs != 0 && (now - s_lastFlushMs) < 400) {
    return;
  }
  s_lastFlushMs = now;
  storageFlushTcSessionPending();
  climateRegulatorFlushPendingSave();
  uiDisplayFlushPendingStorage();
}

void uiBusProcessAppMsg(const AppMsg* msg) {
  if (!msg) {
    return;
  }
  processAppMsg(*msg);
}
