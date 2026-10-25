#include "climate_scheduler.h"

#include "bus_lg_config.h"
#include "bus_rotenso_config.h"
#include "climate_plan.h"
#include "storage_config_nvs.h"
#include "src/bus/bus_rotenso_modbus.h"
#include "src/bus_lg_model.h"
#include "src/bus_lg_protocol.h"
#include "ui_eez_model.h"

#include <Arduino.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

namespace {

bool s_manualOverride = false;
bool s_manualValue = false;

/** Požadovaný tichý režim (okno Night mode), nezávisle na RO 0044H. */
bool s_quietCmd = false;
bool s_quietCmdKnown = false;

/** Práh OAT (°C) — pod ním po prodlevě vypnout quiet. */
float s_quietOatOffC = CLIMATE_QUIET_OAT_OFF_C;
/** Po auto-vypnutí: ZAP zakázán, dokud OAT >= práh + hystereze. */
bool s_quietColdBlocked = false;
uint32_t s_quietColdSinceMs = 0;
bool s_quietColdTiming = false;

constexpr float kOatMinC = -25.0f;
constexpr float kOatMaxC = 10.0f;

bool isNightHour(int hour) {
  if (CLIMATE_TICHY_NOC_START_H == CLIMATE_TICHY_NOC_END_H) {
    return false;
  }
  if (CLIMATE_TICHY_NOC_START_H < CLIMATE_TICHY_NOC_END_H) {
    return hour >= CLIMATE_TICHY_NOC_START_H && hour < CLIMATE_TICHY_NOC_END_H;
  }
  return hour >= CLIMATE_TICHY_NOC_START_H || hour < CLIMATE_TICHY_NOC_END_H;
}

void updatePlanText(int hour, int minute, bool tichyActive, bool autoMode) {
  (void)hour;
  (void)minute;
  (void)tichyActive;
  (void)autoMode;
}

bool queueQuiet(bool on) {
  const uint16_t start = WINDMI_NIGHT_HHMM(0, 0);
  const uint16_t end = on ? WINDMI_NIGHT_HHMM(23, 59) : WINDMI_NIGHT_HHMM(0, 0);
  if (!rotensoBusQueueConfigWrite((uint16_t)WINDMI_REG_NIGHT_START, start)) {
    Serial.println("[T/C] quiet: fronta start plna");
    return false;
  }
  if (!rotensoBusQueueConfigWrite((uint16_t)WINDMI_REG_NIGHT_END, end)) {
    Serial.println("[T/C] quiet: fronta end plna");
    return false;
  }
  lgModelSetQuietNight(on ? 1u : 0u);
  return true;
}

float clampOat(float c) {
  if (c < kOatMinC) {
    return kOatMinC;
  }
  if (c > kOatMaxC) {
    return kOatMaxC;
  }
  return c;
}

void persistOatOff(void) {
  const int16_t x10 = (int16_t)lroundf(s_quietOatOffC * 10.0f);
  storageSaveQuietOatOffTenths(x10);
}

bool outdoorOk(float* outC) {
  if (!mVenkovniOk || isnan(mVenkovniC)) {
    return false;
  }
  if (outC) {
    *outC = mVenkovniC;
  }
  return true;
}

void quietOatProtectTick(void) {
  float oat = 0.0f;
  if (!outdoorOk(&oat)) {
    s_quietColdTiming = false;
    s_quietColdSinceMs = 0;
    return;
  }

  const float offTh = s_quietOatOffC;
  const float onTh = s_quietOatOffC + CLIMATE_QUIET_OAT_HYST_C;

  // Hystereze: odblokovat ZAP až po oteplení.
  if (s_quietColdBlocked && oat >= onTh) {
    s_quietColdBlocked = false;
    Serial.printf("[T/C] quiet OAT odblokovan (%.1f >= %.1f)\n", (double)oat,
                  (double)onTh);
  }

  if (!climateQuietIsOn()) {
    s_quietColdTiming = false;
    s_quietColdSinceMs = 0;
    return;
  }

  // Quiet ZAP a OAT pod prahem → odpočet, pak vypnout.
  if (oat < offTh) {
    const uint32_t now = millis();
    if (!s_quietColdTiming) {
      s_quietColdTiming = true;
      s_quietColdSinceMs = now;
      Serial.printf("[T/C] quiet OAT cold start (%.1f < %.1f), delay %lu s\n",
                    (double)oat, (double)offTh,
                    (unsigned long)(CLIMATE_QUIET_OAT_OFF_DELAY_MS / 1000u));
    } else if ((now - s_quietColdSinceMs) >= CLIMATE_QUIET_OAT_OFF_DELAY_MS) {
      Serial.printf("[T/C] quiet AUTO VYP OAT %.1f < %.1f\n", (double)oat,
                    (double)offTh);
      if (queueQuiet(false)) {
        s_quietCmd = false;
        s_quietCmdKnown = true;
        s_quietColdBlocked = true;
      }
      s_quietColdTiming = false;
      s_quietColdSinceMs = 0;
    }
  } else {
    if (s_quietColdTiming) {
      Serial.printf("[T/C] quiet OAT cold cancel (%.1f >= %.1f)\n", (double)oat,
                    (double)offTh);
    }
    s_quietColdTiming = false;
    s_quietColdSinceMs = 0;
  }
}

}  // namespace

void climateSchedulerInit() {
  s_manualOverride = false;
  s_manualValue = false;
  s_quietCmd = false;
  s_quietCmdKnown = false;
  s_quietColdBlocked = false;
  s_quietColdTiming = false;
  s_quietColdSinceMs = 0;
  s_quietOatOffC = CLIMATE_QUIET_OAT_OFF_C;
  {
    int16_t x10 = 0;
    if (storageLoadQuietOatOffTenths(&x10)) {
      s_quietOatOffC = clampOat((float)x10 * 0.1f);
    }
  }
  updatePlanText(0, 0, false, false);
}

void climateSchedulerTick() {
  quietOatProtectTick();

  // Týdenní plán vlastní sig_utlum (Norm/Útlum/Vyp). Nesmíme ho každou
  // noc přepisovat — jinak při Norm vypadá „útlum“ aktivní skoro pořád.
  if (climatePlanJeAktivni()) {
    return;
  }

  if (!uiEez.cas_platny) {
    if (!s_manualOverride) {
      uiEez.sig_utlum = false;
    }
    updatePlanText(0, 0, uiEez.sig_utlum, false);
    return;
  }

  time_t now = time(nullptr);
  struct tm tmLocal;
  localtime_r(&now, &tmLocal);

  if (s_manualOverride) {
    uiEez.sig_utlum = s_manualValue;
    updatePlanText(tmLocal.tm_hour, tmLocal.tm_min, uiEez.sig_utlum, false);
    return;
  }

#if CLIMATE_TICHY_AUTO_NOC
  uiEez.sig_utlum = isNightHour(tmLocal.tm_hour);
  updatePlanText(tmLocal.tm_hour, tmLocal.tm_min, uiEez.sig_utlum, true);
#else
  updatePlanText(tmLocal.tm_hour, tmLocal.tm_min, uiEez.sig_utlum, false);
#endif
}

bool climateQuietIsOn(void) {
  if (s_quietCmdKnown) {
    return s_quietCmd;
  }
  return lgJeTichyRezimLinAktivni();
}

bool climateQuietColdBlocked(void) { return s_quietColdBlocked; }

float climateQuietOatOffC(void) { return s_quietOatOffC; }

void climateQuietOatOffAdjust(int deltaC) {
  s_quietOatOffC = clampOat(s_quietOatOffC + (float)deltaC);
  persistOatOff();
  Serial.printf("[T/C] quiet OAT prah = %.1f C\n", (double)s_quietOatOffC);
}

void climateQuietSet(bool on) {
  if (on && s_quietColdBlocked) {
    Serial.println("[T/C] quiet ZAP zamitnut — OAT nizka (cold block)");
    return;
  }
  if (s_quietCmdKnown && s_quietCmd == on) {
    return;
  }
  if (queueQuiet(on)) {
    s_quietCmd = on;
    s_quietCmdKnown = true;
    if (on) {
      s_quietColdBlocked = false;
      s_quietColdTiming = false;
    }
    Serial.printf("[T/C] quiet rezim: %s (night window)\n", on ? "ZAP" : "VYP");
  }
}

void climateTichyManualToggle() {
  climateQuietSet(!climateQuietIsOn());
}
