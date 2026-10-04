// ui_eez_model.cpp — EEZ model + sync z LIN
#include "ui_eez_model.h"

#include "bus_lg_config.h"
#include "bus_lg_lin_api.h"
#include "bus_lg_model.h"
#include "bus_lg_protocol.h"

#include "storage_config_nvs.h"
#include "ui_eez_porucha.h"

#include <cstdio>
#include <cstring>
#include <math.h>

UiEezModel uiEez;

namespace {

float uiTeplotaC(uint8_t b, bool platna) {
  if (!platna) {
    return UI_TEPLOTA_NEPLATNA;
  }
  return static_cast<float>(b);
}

unsigned long spPendingWarnMs(void) {
  const unsigned long period = lgA0PeriodMs();
  if (period > 0) {
    const unsigned long adaptive = period + UI_SP_PENDING_MARGIN_MS;
    return adaptive > UI_SP_PENDING_WARN_MS ? adaptive : UI_SP_PENDING_WARN_MS;
  }
  return UI_SP_PENDING_WARN_MS;
}

}  // namespace

void uiEezInit() {
  memset(&uiEez, 0, sizeof(uiEez));
  uiEez.teplota_vody_set = UI_TEPLOTA_NEPLATNA;
  uiEez.teplota_vody_vstup = UI_TEPLOTA_NEPLATNA;
  uiEez.teplota_vody_vystup = UI_TEPLOTA_NEPLATNA;
  uiEez.teplota_vnitrni = UI_TEPLOTA_NEPLATNA;
  uiEez.teplota_venkovni = UI_TEPLOTA_NEPLATNA;
  uiEez.teplota_spad = UI_TEPLOTA_NEPLATNA;
  uiEez.rezim = UI_REZIM_VYSTUPNI_TEPLOTA;
  {
    uint8_t saved = UI_REZIM_VYSTUPNI_TEPLOTA;
    if (storageLoadUiRezim(&saved)) {
      uiEez.rezim = (UiRezimRegulace)saved;
    }
  }
  uiEez.stav_tc = UI_STAV_VYP;
  uiEez.sig_wifi = false;
  uiEez.sig_mqtt = false;
  uiEez.sig_ble = false;
  uiEez.sig_remote = false;
  strncpy(uiEez.cas_text, "--:--", sizeof(uiEez.cas_text));
  strncpy(uiEez.datum_text, "--.--.----", sizeof(uiEez.datum_text));
  uiEez.cas_platny = false;
  strncpy(uiEez.plan_title, "PLÁN VYPNUTÝ", sizeof(uiEez.plan_title));
  strncpy(uiEez.plan_text, "Časový plán je neaktivní", sizeof(uiEez.plan_text));
  strncpy(uiEez.set_wifi_ssid, "---", sizeof(uiEez.set_wifi_ssid));
  strncpy(uiEez.set_wifi_ip, "---", sizeof(uiEez.set_wifi_ip));
  strncpy(uiEez.set_wifi_status, "Odpojeno", sizeof(uiEez.set_wifi_status));
  strncpy(uiEez.set_mqtt_host, "---", sizeof(uiEez.set_mqtt_host));
  strncpy(uiEez.set_mqtt_status, "Odpojeno", sizeof(uiEez.set_mqtt_status));
  uiEez.porucha_text[0] = '\0';
  strncpy(uiEez.set_sys_hint, "Čekám na senzor...", sizeof(uiEez.set_sys_hint));
  uiEez.set_sys_hint[sizeof(uiEez.set_sys_hint) - 1] = '\0';
}

void uiEezNastavCas(const char* cas, const char* datum, bool platny) {
  if (cas) {
    snprintf(uiEez.cas_text, sizeof(uiEez.cas_text), "%s", cas);
  }
  if (datum) {
    snprintf(uiEez.datum_text, sizeof(uiEez.datum_text), "%s", datum);
  }
  uiEez.cas_platny = platny;
}

void uiEezNastavSit(bool wifi, bool mqtt, bool ble) {
  uiEez.sig_wifi = wifi;
  uiEez.sig_mqtt = mqtt;
  uiEez.sig_ble = ble;
}

void uiEezNastavTeplotuVnitrni(float c) { uiEez.teplota_vnitrni = c; }
void uiEezNastavTeplotuVenkovni(float c) { uiEez.teplota_venkovni = c; }

void uiEezSyncFromBus() {
  lgModelLock();

  const bool maA0 = lgMaCerstoA0();
  uint8_t b2 = lgModelA0Bajt(2);
  uint8_t b3 = lgModelA0Bajt(3);

  uint8_t cilova = pozadavekNaZapis ? novaCilovaTeplota : mCilova;

  {
    const uint8_t a0Sp = maA0 ? lgModelA0Bajt(8) : 0;
    const bool a0SpPlatny = maA0 && a0Sp >= 15 && a0Sp <= 65;
    // MAN: SP z Modbus (a0Snap[8] / mCilova) — mCilova platí i bez bajtu 8
    const bool mSpPlatny = maA0 && mCilova >= 15 && mCilova <= 65;
    const uint8_t busSp = a0SpPlatny ? a0Sp : (mSpPlatny ? mCilova : 0);
    const bool busSpPlatny = busSp >= 15 && busSp <= 65;

    if (uiEez.sp_pending != 0) {
      if (busSpPlatny && busSp == uiEez.sp_pending) {
        uiEez.sp_pending = 0;
        uiEez.teplota_vody_set = static_cast<float>(busSp);
      } else {
        uiEez.teplota_vody_set = static_cast<float>(uiEez.sp_pending);
      }
    } else if (busSpPlatny) {
      uiEez.teplota_vody_set = static_cast<float>(busSp);
    } else if (cilova >= 15 && cilova <= 65) {
      uiEez.teplota_vody_set = static_cast<float>(cilova);
    } else if (!uiRezimRegulatorWritesWater(uiEez.rezim)) {
      uiEez.teplota_vody_set = UI_TEPLOTA_NEPLATNA;
    }
  }

  static float s_holdVstup = NAN;
  static float s_holdVystup = NAN;
  // Aktualizuj hold při jakékoli platné hodnotě (nečekej na obě)
  if (!isnan(mVstupniC) && mVstupniOk) {
    s_holdVstup = mVstupniC;
  }
  if (!isnan(mVystupniC) && mVystupniOk) {
    s_holdVystup = mVystupniC;
  }
  const float vstupShow =
      (mVstupniOk && !isnan(mVstupniC)) ? mVstupniC : s_holdVstup;
  const float vystupShow =
      (mVystupniOk && !isnan(mVystupniC)) ? mVystupniC : s_holdVystup;
  // Zobraz i při krátkém výpadku live — hold drží poslední dobré hodnoty
  const bool vstupPlatny = !isnan(vstupShow);
  const bool vystupPlatny = !isnan(vystupShow);
  uiEez.teplota_vody_vstup =
      vstupPlatny ? vstupShow : UI_TEPLOTA_NEPLATNA;
  uiEez.teplota_vody_vystup =
      vystupPlatny ? vystupShow : UI_TEPLOTA_NEPLATNA;

  // Venkovní teplota = Modbus TČ (0001H), stejný hold model jako Tin/Tout.
  static float s_holdVenkovni = NAN;
  if (mVenkovniOk && !isnan(mVenkovniC)) {
    s_holdVenkovni = mVenkovniC;
  }
  const float venkShow =
      (mVenkovniOk && !isnan(mVenkovniC)) ? mVenkovniC : s_holdVenkovni;
  uiEez.teplota_venkovni =
      !isnan(venkShow) ? venkShow : UI_TEPLOTA_NEPLATNA;

  if (vstupPlatny && vystupPlatny) {
    uiEez.teplota_spad = vystupShow - vstupShow;
    if (uiEez.teplota_spad < 0) {
      uiEez.teplota_spad = -uiEez.teplota_spad;
    }
  } else {
    uiEez.teplota_spad = UI_TEPLOTA_NEPLATNA;
  }

  const bool runOn = maA0 && (mMbRunningMode != 0u);
  const bool pendingOn = mMbPowerPending && mMbPowerWantOn;
  const bool pendingOff = mMbPowerPending && !mMbPowerWantOn;
  const bool tcBezi = maA0 && lgJeTcProvoz(b2, b3);

  // CHOD = běžící režim / pending START (ne LG session)
  uiEez.sig_chod = (runOn || pendingOn) && !pendingOff;

  // b2/b3 = syntetika z Modbus (pump/flow/run, komp, defrost, IBH1)
  uiEez.sig_cerpadlo = maA0 && lgJeCerpadloZap(b2);
  uiEez.sig_kompresor = maA0 && lgJeKompresorBezi(b3);
  uiEez.sig_el_topeni = maA0 && lgJeElTopeni(b2);
  uiEez.sig_odmrazovani = maA0 && ((b3 & 0x04) != 0);
  uiEez.sig_tichy_lin = maA0 && lgJeTichyRezimLinAktivni();

  if (pendingOff || (!runOn && !pendingOn)) {
    uiEez.stav_tc = UI_STAV_VYP;
  } else if (pendingOn && !runOn) {
    uiEez.stav_tc = UI_STAV_PRESTART;
  } else if (maA0 && (tcBezi || lgJeCerpadloZap(b2) || runOn)) {
    uiEez.stav_tc = UI_STAV_BEH;
  } else {
    uiEez.stav_tc = UI_STAV_VYP;
  }

  lgModelUnlock();
  uiEezRefreshPorucha();
}

uint32_t uiEezTeplotaVodySetColor(void) {
  if (uiRezimRegulatorWritesWater(uiEez.rezim) || uiEez.sp_pending == 0) {
    return UI_SP_COLOR_OK;
  }
  if ((millis() - uiEez.sp_pending_ms) >= spPendingWarnMs()) {
    return UI_SP_COLOR_WARN;
  }
  return UI_SP_COLOR_PENDING;
}
