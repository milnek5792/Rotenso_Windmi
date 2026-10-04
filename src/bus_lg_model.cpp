#include "bus_lg_model.h"
#include "bus_lg_config.h"
#include "bus_rotenso_config.h"
#include "storage_config_nvs.h"
#include <cstring>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

uint8_t mCilova = 0, mVstupni = 0, mVystupni = 0;
float mVenkovniC = NAN;
float mVstupniC = NAN;
float mVystupniC = NAN;
bool mVenkovniOk = false;
bool mVodaOk = false;
bool mVstupniOk = false;
bool mVystupniOk = false;
uint16_t mMbRunningMode = 0;
bool mMbPowerPending = false;
bool mMbPowerWantOn = false;
bool stavZapnuto = false;
volatile bool pozadavekNaZapis = false;
bool pozadavekZmenaStartu = false;
uint8_t novaCilovaTeplota = 0;
bool bliknuti = false;
bool monitorPozastaven = false;
bool origOvladacDetekovan = false;
bool origTichyRezimLin = false;
bool soloRezimTab5 = false;
bool parallelRezimTab5 = false;
bool drzetStavAktivni = false;
bool cilovyZapnutoTab5 = false;
bool cekameNaOrigStart = false;
bool tcPozadavekZap = false;
uint8_t cilovaTeplotaTab5 = 0;

char posledniStavovyText[64] = "Modbus — cekam na Windmi...";
volatile bool potrebaObnovitDisplej = false;

static SemaphoreHandle_t lgModelMu;
static uint8_t a0Snap[20];
static uint8_t a0SnapLen = 0;
static unsigned long s_casPosledniA0Ms = 0;

static WindmiLiveSnap s_live = {};
static WindmiAlarmSnap s_alarms = {};
static WindmiHpConfigSnap s_hpCfg = {};

void lgModelInit() {
  lgModelMu = xSemaphoreCreateRecursiveMutex();
#if LG_BOARD_7B
  soloRezimTab5 = true;
#endif
  mCilova = 0;
  novaCilovaTeplota = 0;
}

void lgModelRestoreSessionFromNvs() {
  storageInit();
  bool on = false;
  uint8_t sp = 35;
  if (!storageLoadTcSession(&on, &sp)) {
    Serial.println("[BOOT] NVS session — neni ulozena");
    return;
  }
  lgModelLock();
  cilovaTeplotaTab5 = sp;
  // Po FW restartu obnovit START, pokud byl předtím ON (TČ může dál běžet).
  cilovyZapnutoTab5 = on;
  drzetStavAktivni = true;
  tcPozadavekZap = on;
  if (sp >= 15 && sp <= 65) {
    mCilova = sp;
    novaCilovaTeplota = sp;
  }
  lgModelUnlock();
  Serial.printf("[BOOT] NVS SP=%u session=%s (START az po live Modbus)\n",
                (unsigned)sp, on ? "ON" : "OFF");
  // Nequeueovat zápis hned — dokud nefunguje čtení, START blokuje celý bus.
}

void lgModelLock() {
  if (lgModelMu) { xSemaphoreTakeRecursive(lgModelMu, portMAX_DELAY); }
}

void lgModelUnlock() {
  if (lgModelMu) { xSemaphoreGiveRecursive(lgModelMu); }
}

void nastavStavovyText(const char* text) {
  lgModelLock();
  snprintf(posledniStavovyText, sizeof(posledniStavovyText), "%s", text);
  lgModelUnlock();
}

void lgModelSnapA0(const uint8_t* data, uint8_t len) {
  lgModelLock();
  lgModelSnapA0Locked(data, len);
  lgModelUnlock();
}

void lgModelSnapA0Locked(const uint8_t* data, uint8_t len) {
  if (len > sizeof(a0Snap)) {
    len = sizeof(a0Snap);
  }
  memcpy(a0Snap, data, len);
  a0SnapLen = len;
  s_casPosledniA0Ms = millis();
}

uint8_t lgModelA0Bajt(uint8_t idx, uint8_t vychozi) {
  lgModelLock();
  uint8_t v = (a0SnapLen > idx) ? a0Snap[idx] : vychozi;
  lgModelUnlock();
  return v;
}

bool lgMaCerstoA0(uint32_t maxAgeMs) {
  if (maxAgeMs == 0) {
    maxAgeMs = LG_A0_FRESH_MS;
  }
  lgModelLock();
  const unsigned long t = s_casPosledniA0Ms;
  lgModelUnlock();
  if (t == 0) {
    return false;
  }
  return (millis() - t) < maxAgeMs;
}

void lgModelTouchLive(void) {
  lgModelLock();
  s_casPosledniA0Ms = millis();
  lgModelUnlock();
}

static uint8_t tempToLegacyByte(float c) {
  if (isnan(c) || c < 0.0f) {
    return 0;
  }
  if (c > 99.0f) {
    return 99;
  }
  return (uint8_t)(c + 0.5f);
}

void lgModelSetMbTemps(float outdoorC, bool outdoorOk, float inletC, bool inletOk,
                       float outletC, bool outletOk) {
  lgModelLock();
  s_casPosledniA0Ms = millis();
  if (outdoorOk) {
    mVenkovniC = outdoorC;
    mVenkovniOk = true;
  }
  if (inletOk) {
    mVstupniC = inletC;
    mVstupniOk = true;
    mVstupni = tempToLegacyByte(inletC);
  }
  if (outletOk) {
    mVystupniC = outletC;
    mVystupniOk = true;
    mVystupni = tempToLegacyByte(outletC);
  }
  mVodaOk = mVstupniOk && mVystupniOk;
  lgModelUnlock();
}

void lgModelSetMbStatus(uint16_t settingMode, uint16_t runningMode,
                        int16_t compFreqX10, uint16_t pumpSpeed,
                        uint16_t quietNight, uint16_t loadOutput,
                        bool loadOutputOk, uint16_t waterFlowX100) {
  lgModelLock();
  s_casPosledniA0Ms = millis();
  mMbRunningMode = runningMode;
  s_live.valid = true;
  s_live.setting_mode = settingMode;
  s_live.running_mode = runningMode;
  s_live.comp_freq_x10 = compFreqX10;
  s_live.pump_speed = pumpSpeed;
  s_live.quiet_night = quietNight;
  s_live.load_output = loadOutput;
  s_live.load_ok = loadOutputOk;
  s_live.water_flow_x100 = waterFlowX100;

  // Potvrzení START/STOP: 002DH
  bool confirmed = false;
  bool confirmedOn = false;
  if (mMbPowerPending) {
    const bool runOn = (runningMode != 0u);
    if (mMbPowerWantOn == runOn) {
      confirmed = true;
      confirmedOn = runOn;
      mMbPowerPending = false;
      stavZapnuto = runOn;
    }
  } else {
    stavZapnuto = (runningMode != 0u);
  }

  uint8_t b2 = 0;
  uint8_t b3 = 0;

  // Čerpadlo: vnitřní rychlost, průtok, nebo aktivní provozní režim (ne Off)
  const bool pumpOn =
      (pumpSpeed > 0) || (waterFlowX100 > 0) || (runningMode != 0u);
  if (pumpOn) {
    b2 |= 0x02;  // lgJeCerpadloZap
  }

  const bool defrost = (runningMode == (uint16_t)WINDMI_RUN_DEFROST);
  if (defrost) {
    b3 |= 0x04;  // sig_odmrazovani
  }

  // Frekvence: HA „Hz“, manuál někdy ×10 — oboje >0 ⇒ běží
  const bool compOn = (compFreqX10 > 0);
  if (compOn) {
    b3 |= 0x0A;  // ZAP + BEZI → lgJeKompresorBezi / lgJeTcProvoz
  } else if (runningMode != 0u) {
    b3 |= 0x08;  // cyklus / příprava bez kompresoru
  }

  if (loadOutputOk && (loadOutput & WINDMI_LOAD_IBH1_BIT) != 0u) {
    b2 |= 0x04;  // lgJeElTopeni ← IBH1
  }

  if (a0SnapLen < 4u) {
    // Jen rozšířit — nemazat případný SP v [8]
    for (uint8_t i = a0SnapLen; i < 4u; ++i) {
      a0Snap[i] = 0;
    }
    a0SnapLen = 4;
  }
  a0Snap[1] = (quietNight == 1u) ? 0x31u : 0x32u;
  a0Snap[2] = b2;
  a0Snap[3] = b3;
  origTichyRezimLin = (quietNight == 1u);

  lgModelUnlock();
  if (confirmed) {
    Serial.printf("[MB] power confirm run=%u -> %s\n", (unsigned)runningMode,
                  confirmedOn ? "ON" : "OFF");
  }
}

void lgModelSetMbLiveExtras(int16_t reqCompFreqX10, bool reqCompOk) {
  lgModelLock();
  s_live.req_comp_freq_x10 = reqCompFreqX10;
  s_live.req_comp_ok = reqCompOk;
  lgModelUnlock();
}

void lgModelSetMbHpConfig(const WindmiHpConfigSnap* cfg) {
  if (!cfg) {
    return;
  }
  lgModelLock();
  s_hpCfg = *cfg;
  lgModelUnlock();
}

void lgModelReadLiveSnap(WindmiLiveSnap* out) {
  if (!out) {
    return;
  }
  lgModelLock();
  *out = s_live;
  lgModelUnlock();
}

void lgModelReadHpConfigSnap(WindmiHpConfigSnap* out) {
  if (!out) {
    return;
  }
  lgModelLock();
  *out = s_hpCfg;
  lgModelUnlock();
}

void lgModelSetMbAlarms(const uint16_t bm[4]) {
  if (!bm) {
    return;
  }
  lgModelLock();
  s_alarms.valid = true;
  for (int i = 0; i < 4; ++i) {
    s_alarms.bm[i] = bm[i];
  }
  lgModelUnlock();
}

void lgModelReadAlarmSnap(WindmiAlarmSnap* out) {
  if (!out) {
    return;
  }
  lgModelLock();
  *out = s_alarms;
  lgModelUnlock();
}

bool lgModelHasHpAlarm(void) {
  lgModelLock();
  const bool ok = s_alarms.valid;
  const uint16_t a = s_alarms.bm[0];
  const uint16_t b = s_alarms.bm[1];
  const uint16_t c = s_alarms.bm[2];
  const uint16_t d = s_alarms.bm[3];
  lgModelUnlock();
  return ok && ((a | b | c | d) != 0u);
}

void lgModelSetMbWaterSp(uint8_t spC) {
  if (spC < 15 || spC > 65) {
    return;
  }
  lgModelLock();
  mCilova = spC;
  s_casPosledniA0Ms = millis();
  if (a0SnapLen < 9u) {
    for (uint8_t i = a0SnapLen; i < 9u; ++i) {
      a0Snap[i] = 0;
    }
    a0SnapLen = 9;
  }
  a0Snap[8] = spC;
  lgModelUnlock();
}

void lgModelReadUiSnap(LgModelUiSnap* out) {
  if (!out) {
    return;
  }
  memset(out, 0, sizeof(*out));
  lgModelLock();
  const unsigned long a0Ms = s_casPosledniA0Ms;
  out->lin_live = (a0Ms != 0) && ((millis() - a0Ms) < LG_A0_FRESH_MS);
  if (a0SnapLen > 2) {
    out->b2 = a0Snap[2];
  }
  if (a0SnapLen > 3) {
    out->b3 = a0Snap[3];
  }
  if (a0SnapLen > 8) {
    out->a0_sp = a0Snap[8];
  }
  out->m_vstupni = mVstupni;
  out->m_vystupni = mVystupni;
  out->m_cilova = mCilova;
  out->nova_cilova = novaCilovaTeplota;
  out->pozadavek_zapis = pozadavekNaZapis;
  out->cilovy_zapnuto = cilovyZapnutoTab5;
  out->cekame_orig = cekameNaOrigStart;
  out->tc_pozadavek = tcPozadavekZap;
  lgModelUnlock();
}
