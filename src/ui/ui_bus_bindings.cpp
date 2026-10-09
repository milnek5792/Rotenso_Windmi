// ui_bus_bindings.cpp — EEZ akce → Modbus zápis + sync model → UI
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
#include "ui_eez_nav.h"
#include "src/net_mqtt_client.h"

#include <Arduino.h>
#include <esp_log.h>
#include <math.h>
#include <math.h>

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

/** Pokoj/ekvitermá → jen regulátor; ruční → HMI / MQTT / plán. */
bool allowWaterSpWrite(UiSpSource src) {
  if (uiRezimRegulatorWritesWater(uiEez.rezim)) {
    return src == UI_SP_SRC_REGULATOR;
  }
  return src == UI_SP_SRC_HMI || src == UI_SP_SRC_MQTT || src == UI_SP_SRC_PLAN;
}

const char* rezimName(UiRezimRegulace r) {
  switch (r) {
    case UI_REZIM_AUTO:
      return "POKOJ";
    case UI_REZIM_EKVITERM:
      return "EKVITERM";
    default:
      return "RUCNI";
  }
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
  if (pozadavekNaZapis && !pozadavekZmenaStartu) {
    return clampWaterC(novaCilovaTeplota);
  }
  if (uiEez.sp_pending != 0) {
    return clampWaterC(uiEez.sp_pending);
  }
  if (mCilova >= kWaterMinC && mCilova <= kWaterMaxC) {
    return mCilova;
  }
  // Zobrazovaná hodnota (po poll SP z TČ)
  if (!isnan(uiEez.teplota_vody_set) &&
      uiEez.teplota_vody_set > (UI_TEPLOTA_NEPLATNA + 1.0f)) {
    const int shown = (int)(uiEez.teplota_vody_set + 0.5f);
    if (shown >= (int)kWaterMinC && shown <= (int)kWaterMaxC) {
      return (uint8_t)shown;
    }
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
  lgModelLock();
  const uint8_t t = aktualniCilovaTeplota();
  lgModelUnlock();

  if (cekameNaOrigStart) {
    lgUkonciCekaniProStop();
  }
  s_planSessionHold = false;
  tcPozadavekZap = false;
  lgNastavDrzenyStav(t, false);

  lgModelLock();
  novaCilovaTeplota = t;
  pozadavekZmenaStartu = true;
  pozadavekNaZapis = true;
  mMbPowerPending = true;
  mMbPowerWantOn = false;
  lgModelUnlock();

  uiEez.sig_chod = false;
  uiEez.stav_tc = UI_STAV_VYP;
  storageRequestSaveTcSession(false, t);
  ESP_LOGI(TAG, "STOP -> Modbus 002C=0 T=%u", (unsigned)t);
}

/** Dočasné vypnutí z plánu — Modbus STOP, session START se nemaže. */
void provedPlanSuspend() {
  lgModelLock();
  const uint8_t t = aktualniCilovaTeplota();
  lgModelUnlock();

  const bool wasSession = drzenyZapnuty() || s_planSessionHold;

  if (cekameNaOrigStart) {
    lgUkonciCekaniProStop();
  }
  tcPozadavekZap = false;

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
  mMbPowerPending = true;
  mMbPowerWantOn = false;
  lgModelUnlock();

  uiEez.sig_chod = false;
  uiEez.stav_tc = UI_STAV_VYP;
  ESP_LOGI(TAG, "PLAN VYP -> Modbus STOP T=%u (session %s)", (unsigned)t,
           wasSession ? "HOLD/START" : "OFF");
}

void provedStart() {
  lgModelLock();
  const bool uzDrzeny = drzenyZapnuty();
  const bool uzBezi = (mMbRunningMode != 0u) || stavZapnuto;
  uint8_t t = aktualniCilovaTeplota();
  lgModelUnlock();

  s_planSessionHold = false;
  // Ruční START přebije plánové VYP (jinak další tick zase STOP).
  climateRegulatorSetPlanStop(false);

  if (uiRezimRegulatorWritesWater(uiEez.rezim)) {
    RegulatorSnapshot snap{};
    climateRegulatorGetSnapshot(&snap);
    t = snap.t_water_c;
  }

  if (uzDrzeny && cilovyZapnutoTab5 && uzBezi && !mMbPowerPending) {
    ESP_LOGI(TAG, "START ignorovan — uz bezi");
    return;
  }

  // Session ON / pending, ale TČ ještě neběží → znovu zařadit zápis 002C (ne STOP)
  if ((uzDrzeny || mMbPowerPending) && !uzBezi) {
    lgModelLock();
    novaCilovaTeplota = t;
    mCilova = t;
    tcPozadavekZap = true;
    lgNastavDrzenyStav(t, true);
    pozadavekZmenaStartu = true;
    pozadavekNaZapis = true;
    mMbPowerPending = true;
    mMbPowerWantOn = true;
    lgModelUnlock();
    // CHOD až po run!=0 (002DH); tady jen fronta Heat
    uiEez.stav_tc = UI_STAV_PRESTART;
    uiEez.sp_pending = t;
    uiEez.sp_pending_ms = millis();
    storageRequestSaveTcSession(true, t);
    ESP_LOGI(TAG, "START retry -> Modbus 002C=Heat T=%u", (unsigned)t);
    return;
  }

  // Už běží na TČ — jen adoptovat session + případně SP
  if (uzBezi && !mMbPowerPending) {
    lgModelLock();
    const uint8_t a0Sp = lgModelA0Bajt(8);
    novaCilovaTeplota = t;
    mCilova = t;
    tcPozadavekZap = true;
    lgNastavDrzenyStav(t, true);
    if (a0Sp != t) {
      pozadavekNaZapis = true;
      // necháme pozadavekZmenaStartu jak je — neforcing false
    }
    lgModelUnlock();
    uiEez.stav_tc = UI_STAV_BEH;
    uiEez.sp_pending = t;
    uiEez.sp_pending_ms = millis();
    if (uiRezimRegulatorWritesWater(uiEez.rezim)) {
      climateRegulatorRequestImmediateTick();
    }
    storageRequestSaveTcSession(true, t);
    ESP_LOGI(TAG, "START adopt (run!=0) T=%u", (unsigned)t);
    return;
  }

  lgModelLock();
  novaCilovaTeplota = t;
  mCilova = t;
  tcPozadavekZap = true;
  lgNastavDrzenyStav(t, true);
  pozadavekZmenaStartu = true;
  pozadavekNaZapis = true;
  mMbPowerPending = true;
  mMbPowerWantOn = true;
  lgModelUnlock();

  uiEez.stav_tc = UI_STAV_PRESTART;
  uiEez.sp_pending = t;
  uiEez.sp_pending_ms = millis();

  // SP až po zařazení Heat — ImmediateTick nesmí zrušit 002C (viz provedTeplotaAbsolutni).
  if (uiRezimRegulatorWritesWater(uiEez.rezim)) {
    climateRegulatorRequestImmediateTick();
  }

  storageRequestSaveTcSession(true, t);
  ESP_LOGI(TAG, "START -> Modbus 002C=Heat T=%u %s", (unsigned)t,
           rezimName(uiEez.rezim));
}

void provedStartStopToggle() {
  // Session HOLD / desired ON / TČ už běží → STOP
  if (drzenyZapnuty() || s_planSessionHold || mMbRunningMode != 0u ||
      stavZapnuto || mMbPowerPending) {
    if (mMbPowerPending && mMbPowerWantOn && mMbRunningMode == 0u) {
      // pending START ještě neběží — zruš START = STOP
    }
    provedStop();
  } else {
    provedStart();
  }
}

void provedTeplotaAbsolutni(uint8_t nova, UiSpSource src) {
  nova = clampWaterC(nova);
  if (!allowWaterSpWrite(src)) {
    ESP_LOGW(TAG, "SP vody %u blokovan (src=%s rezim=%s)", (unsigned)nova,
             spSrcName(src), rezimName(uiEez.rezim));
    return;
  }

  // Ruční HMI/MQTT → Modbus 0191H (START není nutný)
  if (!uiRezimRegulatorWritesWater(uiEez.rezim) &&
      (src == UI_SP_SRC_HMI || src == UI_SP_SRC_MQTT)) {
    lgModelLock();
    novaCilovaTeplota = nova;
    mCilova = nova;
    if (drzetStavAktivni) {
      cilovaTeplotaTab5 = nova;
    }
    // Nesmí zrušit pending Heat/OFF — jinak START jen rozsvítí CHOD bez 002C.
    pozadavekNaZapis = true;
    lgModelUnlock();

    uiEez.teplota_vody_set = static_cast<float>(nova);
    uiEez.sp_pending = nova;
    uiEez.sp_pending_ms = millis();
    potrebaObnovitDisplej = true;
    netMqttNotifySetpointChanged();

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
  netMqttNotifySetpointChanged();

  ESP_LOGI(TAG, "setpoint cmd -> %u C src=%s zap=%d rezim=%s", (unsigned)nova,
           spSrcName(src), (int)zap, rezimName(uiEez.rezim));

  // Regulátor + T/C neběží: jen cmd pro další START
  if (uiRezimRegulatorWritesWater(uiEez.rezim) && !zap) {
    ESP_LOGI(TAG, "REG SP %u C — bez zapisu (T/C nebezi)", (unsigned)nova);
    return;
  }

  lgModelLock();
  // Jen SP — ponech pozadavekZmenaStartu (Heat má prioritu v processOneWrite).
  pozadavekNaZapis = true;
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
  if (src == UI_SP_SRC_HMI) {
    climateRegulatorRequestImmediateTick();
  }
  potrebaObnovitDisplej = true;
  netMqttNotifySetpointChanged();
  ESP_LOGI(TAG, "Auto room SP -> %.1f (src=%s)",
           (double)climateRegulatorGetConfig()->room_sp_c, spSrcName(src));
}

void provedRoomSpAbs(float c, UiSpSource src) {
  if (!allowRoomSpWrite(src)) {
    ESP_LOGW(TAG, "room SP abs blokovan (src=%s)", spSrcName(src));
    return;
  }
  climateRegulatorSetRoomSp(c);
  if (src == UI_SP_SRC_HMI) {
    climateRegulatorRequestImmediateTick();
  }
  potrebaObnovitDisplej = true;
  netMqttNotifySetpointChanged();
  ESP_LOGI(TAG, "Auto room SP abs -> %.1f (src=%s)",
           (double)climateRegulatorGetConfig()->room_sp_c, spSrcName(src));
}

void processAppMsg(const AppMsg& msg) {
  const bool roomMode = (uiEez.rezim == UI_REZIM_AUTO);
  const bool ekvMode = (uiEez.rezim == UI_REZIM_EKVITERM);

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
      if (roomMode) {
        provedRoomSpAbs((float)msg.arg / 10.0f, msg.src);
      } else if (ekvMode) {
        // Abs. SP vody → nastav korekci (křivka bez offsetu + korekce ≈ desired)
        RegulatorConfig* cfg = climateRegulatorGetConfigMutable();
        const float outC =
            (mVenkovniOk && !isnan(mVenkovniC)) ? mVenkovniC : 0.0f;
        const float bare =
            climateRegulatorEquithermWaterAt(outC) - cfg->offset_c;
        float off = (float)msg.arg - bare;
        if (off < REG_EQ_OFFSET_MIN_C) {
          off = REG_EQ_OFFSET_MIN_C;
        }
        if (off > REG_EQ_OFFSET_MAX_C) {
          off = REG_EQ_OFFSET_MAX_C;
        }
        cfg->offset_c = off;
        climateRegulatorRequestSave();
        climateRegulatorRequestImmediateTick();
      } else {
        provedTeplotaAbsolutni((uint8_t)msg.arg, msg.src);
      }
      break;
    case APP_CMD_SETPOINT_DELTA:
      if (roomMode) {
        provedRoomSpZmena((float)msg.arg / 10.0f, msg.src);
      } else if (ekvMode) {
        climateRegulatorAdjustOffset((float)msg.arg);
      } else {
        provedTeplotaZmena(msg.arg, msg.src);
      }
      break;
    case APP_CMD_SET_MODE:
      uiBusSetRegulationMode((uint8_t)msg.arg);
      ESP_LOGI(TAG, "queue → mode %s (src=%s)", rezimName(uiEez.rezim),
               spSrcName(msg.src));
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
      } else if (uiEez.rezim == UI_REZIM_EKVITERM) {
        climateRegulatorAdjustOffset(1.0f);
      } else {
        provedTeplotaZmena(1, UI_SP_SRC_HMI);
      }
      break;
    case UI_AKCE_TEPLOTA_MINUS:
      if (uiEez.rezim == UI_REZIM_AUTO) {
        provedRoomSpZmena(-0.5f, UI_SP_SRC_HMI);
      } else if (uiEez.rezim == UI_REZIM_EKVITERM) {
        climateRegulatorAdjustOffset(-1.0f);
      } else {
        provedTeplotaZmena(-1, UI_SP_SRC_HMI);
      }
      break;
    case UI_AKCE_REZIM_PREPNOUT: {
      // Pokoj → Ekvitermá → Ruční → Pokoj
      const UiRezimRegulace prev = uiEez.rezim;
      if (prev == UI_REZIM_AUTO) {
        uiEez.rezim = UI_REZIM_EKVITERM;
      } else if (prev == UI_REZIM_EKVITERM) {
        uiEez.rezim = UI_REZIM_VYSTUPNI_TEPLOTA;
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
      }
      if (uiRezimRegulatorWritesWater(uiEez.rezim)) {
        climateRegulatorRequestImmediateTick();
        if (!drzenyZapnuty()) {
          ESP_LOGI(TAG, "%s — ceka START", rezimName(uiEez.rezim));
        }
      }
      uiBusPersistRezim();
      ESP_LOGI(TAG, "rezim -> %s", rezimName(uiEez.rezim));
      break;
    }
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
  } else if (uiEez.rezim == UI_REZIM_EKVITERM) {
    climateRegulatorAdjustOffset((float)deltaC);
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

void uiBusQueueSetRegulationMode(uint8_t rezim) {
  appCmdEnqueueRegMode(rezim, UI_SP_SRC_MQTT);
}

void uiBusPlanApplyStart(void) {
  provedStart();
}

void uiBusPlanApplyStop(void) {
  // Ne jako tlačítko STOP — session START zůstává (resume po konci VYP)
  provedPlanSuspend();
}

void uiBusPlanApplySetpoint(uint8_t teplotaC) {
  // Ruční: přímý SP vody. Pokoj/ekviterm: plán používá offset/VYP, ne tuto cestu.
  provedTeplotaAbsolutni(teplotaC, UI_SP_SRC_PLAN);
}

bool uiBusSessionIsOn(void) {
  return drzenyZapnuty() || s_planSessionHold;
}

bool uiBusSetRegulationAuto(bool enable) {
  return uiBusSetRegulationMode(enable ? (uint8_t)UI_REZIM_AUTO
                                       : (uint8_t)UI_REZIM_VYSTUPNI_TEPLOTA);
}

bool uiBusSetRegulationMode(uint8_t rezim) {
  if (rezim > (uint8_t)UI_REZIM_EKVITERM) {
    return false;
  }
  const UiRezimRegulace prev = uiEez.rezim;
  uiEez.rezim = (UiRezimRegulace)rezim;
  if (prev != uiEez.rezim) {
    uiBusPersistRezim();
    if (uiRezimRegulatorWritesWater(uiEez.rezim)) {
      climateRegulatorRequestImmediateTick();
    }
  }
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
  // Na obrazovce plánu nepsat NVS — flash bliká LVGL; save při uiPlanFlushSave (odchod).
  if (!uiIsPlanScreen()) {
    climatePlanFlushPendingSave();
  }
  uiDisplayFlushPendingStorage();
}

void uiBusProcessAppMsg(const AppMsg* msg) {
  if (!msg) {
    return;
  }
  processAppMsg(*msg);
}
