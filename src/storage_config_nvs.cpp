#include "storage_config_nvs.h"

#include "app_serial_trace.h"

#include "climate_plan.h"
#include "climate_regulator.h"
#include "h2_uart_protocol.h"

#include <Arduino.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>

namespace {

Preferences s_prefs;
bool s_open = false;
SemaphoreHandle_t s_nvsMux = nullptr;
bool s_tcSessionDirty = false;
bool s_tcSessionOnPending = false;
uint8_t s_tcSessionSpPending = 35;

constexpr const char* kNs = "windmi";
constexpr const char* kLegacyNs = "lg_therma";
constexpr const char* kKeyLegacyCleared = "lg_clr";
constexpr const char* kKeyWifiEn = "wifi_en";
constexpr const char* kKeyWifiSsid = "wifi_ssid";
constexpr const char* kKeyWifiPass = "wifi_pass";
constexpr const char* kKeyMqttEn = "mqtt_en";
constexpr const char* kKeyBlPct = "bl_pct";
constexpr const char* kKeyBlSleep = "bl_sleep";
constexpr const char* kKeyPlan = "plan_cfg";
constexpr const char* kKeyReg = "reg_cfg";
constexpr const char* kKeyRoomSpX10 = "room_sp_x10";
constexpr const char* kKeyUiRezim = "ui_rezim";
constexpr const char* kKeyTcOn = "tc_on";
constexpr const char* kKeyTcSp = "tc_sp";
constexpr const char* kKeyBleRoomMac = "ble_room";
constexpr const char* kKeyEnMeta = "en_meta";
constexpr const char* kKeyEnPwr0 = "en_p0";
constexpr const char* kKeyEnPwr1 = "en_p1";
constexpr const char* kKeyEnPwr2 = "en_p2";
constexpr const char* kKeyEnPwr3 = "en_p3";
constexpr const char* kKeyEnPwr4 = "en_p4";
constexpr const char* kKeyEnPwr5 = "en_p5";
constexpr const char* kKeyEnPwr6 = "en_p6";
constexpr uint32_t kPlanMagic = 0x504C414Eu;
/** v5 = pevný packed payload (nezávislý na paddingu PlanTydenConfig v RAM). */
constexpr uint16_t kPlanVersion = 5;
constexpr uint16_t kPlanVersionMinCompat = 2;

#pragma pack(push, 1)
struct PlanObdobiCasStored {
  uint8_t zh;
  uint8_t zm;
  uint8_t kh;
  uint8_t km;
  uint8_t cas_rezim;
};

struct PlanBunkaStored {
  uint8_t akce;
  uint8_t utlum_stupne;
};

struct PlanTydenConfigStored {
  uint8_t aktivni;
  PlanObdobiCasStored obdobi[PLAN_POCET_OBDOBI];
  PlanBunkaStored tabulka[PLAN_POCET_DNU][PLAN_POCET_OBDOBI];
};

struct PlanObdobiCasStoredV3 {
  uint8_t zh;
  uint8_t zm;
  uint8_t kh;
  uint8_t km;
};

struct PlanTydenConfigStoredV3 {
  uint8_t aktivni;
  PlanObdobiCasStoredV3 obdobi[PLAN_POCET_OBDOBI];
  PlanBunkaStored tabulka[PLAN_POCET_DNU][PLAN_POCET_OBDOBI];
};
#pragma pack(pop)

constexpr size_t kPlanPayloadPacked = sizeof(PlanTydenConfigStored);
constexpr size_t kPlanPayloadV3 = sizeof(PlanTydenConfigStoredV3);

void planConfigToStored(const PlanTydenConfig* src, PlanTydenConfigStored* dst) {
  if (!src || !dst) {
    return;
  }
  memset(dst, 0, sizeof(*dst));
  dst->aktivni = src->aktivni ? 1 : 0;
  for (int o = 0; o < PLAN_POCET_OBDOBI; ++o) {
    dst->obdobi[o].zh = src->obdobi[o].zacatek.hodina;
    dst->obdobi[o].zm = src->obdobi[o].zacatek.minuta;
    dst->obdobi[o].kh = src->obdobi[o].konec.hodina;
    dst->obdobi[o].km = src->obdobi[o].konec.minuta;
    dst->obdobi[o].cas_rezim = static_cast<uint8_t>(src->obdobi[o].cas_rezim);
  }
  for (int d = 0; d < PLAN_POCET_DNU; ++d) {
    for (int o = 0; o < PLAN_POCET_OBDOBI; ++o) {
      dst->tabulka[d][o].akce = static_cast<uint8_t>(src->tabulka[d][o].akce);
      dst->tabulka[d][o].utlum_stupne = src->tabulka[d][o].utlum_stupne;
    }
  }
}

void planStoredToConfig(const PlanTydenConfigStored* src, PlanTydenConfig* dst) {
  if (!src || !dst) {
    return;
  }
  dst->aktivni = src->aktivni != 0;
  for (int o = 0; o < PLAN_POCET_OBDOBI; ++o) {
    dst->obdobi[o].zacatek.hodina = src->obdobi[o].zh;
    dst->obdobi[o].zacatek.minuta = src->obdobi[o].zm;
    dst->obdobi[o].konec.hodina = src->obdobi[o].kh;
    dst->obdobi[o].konec.minuta = src->obdobi[o].km;
    const uint8_t cr = src->obdobi[o].cas_rezim;
    dst->obdobi[o].cas_rezim =
        (cr == PLAN_CAS_OD_DELKA) ? PLAN_CAS_OD_DELKA : PLAN_CAS_OD_DO;
  }
  for (int d = 0; d < PLAN_POCET_DNU; ++d) {
    for (int o = 0; o < PLAN_POCET_OBDOBI; ++o) {
      dst->tabulka[d][o].akce = static_cast<PlanAkce>(src->tabulka[d][o].akce);
      dst->tabulka[d][o].utlum_stupne = src->tabulka[d][o].utlum_stupne;
    }
  }
}

void planStoredV3ToConfig(const PlanTydenConfigStoredV3* src, PlanTydenConfig* dst) {
  if (!src || !dst) {
    return;
  }
  dst->aktivni = src->aktivni != 0;
  for (int o = 0; o < PLAN_POCET_OBDOBI; ++o) {
    dst->obdobi[o].zacatek.hodina = src->obdobi[o].zh;
    dst->obdobi[o].zacatek.minuta = src->obdobi[o].zm;
    dst->obdobi[o].konec.hodina = src->obdobi[o].kh;
    dst->obdobi[o].konec.minuta = src->obdobi[o].km;
    dst->obdobi[o].cas_rezim = PLAN_CAS_OD_DELKA;
  }
  for (int d = 0; d < PLAN_POCET_DNU; ++d) {
    for (int o = 0; o < PLAN_POCET_OBDOBI; ++o) {
      dst->tabulka[d][o].akce = static_cast<PlanAkce>(src->tabulka[d][o].akce);
      dst->tabulka[d][o].utlum_stupne = src->tabulka[d][o].utlum_stupne;
    }
  }
}

bool decodePlanPayload(const uint8_t* payload, size_t payloadLen, uint16_t version,
                       PlanTydenConfig* cfg) {
  if (!payload || !cfg || payloadLen == 0) {
    return false;
  }
  if (version < kPlanVersionMinCompat) {
    return false;
  }

  // v5+: vždy pevný packed layout (nikdy raw memcpy — sizeof(PlanTydenConfig) se smí rovnat).
  if (version >= 5) {
    if (payloadLen < kPlanPayloadPacked) {
      return false;
    }
    PlanTydenConfigStored packed{};
    memcpy(&packed, payload, sizeof(packed));
    planStoredToConfig(&packed, cfg);
    return true;
  }

  // v2–v4: jen přesná shoda délky (žádné „>=“ — to kazilo blob a migrate ho přepsal).
  if (payloadLen == kPlanPayloadPacked) {
    PlanTydenConfigStored packed{};
    memcpy(&packed, payload, sizeof(packed));
    planStoredToConfig(&packed, cfg);
    return true;
  }
  if (payloadLen == kPlanPayloadV3) {
    PlanTydenConfigStoredV3 packed{};
    memcpy(&packed, payload, sizeof(packed));
    planStoredV3ToConfig(&packed, cfg);
    return true;
  }
  if (payloadLen == sizeof(PlanTydenConfig)) {
    memcpy(cfg, payload, sizeof(PlanTydenConfig));
    return true;
  }
  return false;
}
constexpr uint32_t kRegMagic = 0x52454731u;  // REG1
constexpr uint16_t kRegVersion = 5;
constexpr uint32_t kSleepOptsSec[] = {0, 60, 120, 300, 600, 1800};

class NvsLock {
 public:
  NvsLock() : taken_(s_nvsMux && xSemaphoreTake(s_nvsMux, portMAX_DELAY) == pdTRUE) {}
  ~NvsLock() {
    if (taken_) {
      xSemaphoreGive(s_nvsMux);
    }
  }

 private:
  bool taken_;
};

bool isValidSleepTimeoutSec(uint32_t sec) {
  for (uint32_t v : kSleepOptsSec) {
    if (v == sec) {
      return true;
    }
  }
  return false;
}

/** NVS v4 — venkovní body + bias_pct (nepoužito v regulaci). */
struct RegulatorConfigV4 {
  float room_sp_c;
  float t_out_cold_c;
  float u_cold_pct;
  float t_out_warm_c;
  float u_warm_pct;
  float kp;
  float ki;
  float kd;
  float bias_pct;
  float trim_limit_pct;
  float deadband_c;
  uint8_t use_equitherm;
  uint8_t _pad[3];
};

void migrateRegulatorV4ToV5(const RegulatorConfigV4* old, RegulatorConfig* cfg) {
  if (!old || !cfg) {
    return;
  }
  cfg->room_sp_c = old->room_sp_c;
  cfg->t_water_cold_c = REG_EQ_WATER_COLD_DEFAULT_C;
  cfg->t_water_warm_c = REG_EQ_WATER_WARM_DEFAULT_C;
  cfg->offset_c = 0.0f;
  cfg->kp = old->kp;
  cfg->ki = old->ki;
  cfg->kd = old->kd;
  cfg->trim_limit_pct = old->trim_limit_pct;
  cfg->deadband_c = old->deadband_c;
  cfg->use_equitherm = old->use_equitherm;
  cfg->_pad[0] = cfg->_pad[1] = cfg->_pad[2] = 0;
}

void clearLegacyLgThermaNsOnce() {
  // Nejdřív otevřít windmi — flag, ať clear neběží při každém storageInit.
  if (!s_prefs.begin(kNs, false)) {
    Serial.println("[NVS] begin(windmi) FAIL");
    return;
  }
  s_open = true;
  if (s_prefs.getBool(kKeyLegacyCleared, false)) {
    return;
  }

  Preferences legacy;
  if (legacy.begin(kLegacyNs, false)) {
    APP_SLOG_LN("[NVS] clearing legacy lg_therma (jednou)...");
    legacy.clear();
    legacy.end();
    APP_SLOG_LN("[NVS] cleared legacy namespace lg_therma");
  }
  s_prefs.putBool(kKeyLegacyCleared, true);
}

void ensureOpen() {
  if (s_open) {
    return;
  }
  if (!s_prefs.begin(kNs, false)) {
    Serial.println("[NVS] begin(windmi) FAIL");
    return;
  }
  s_open = true;
}

}  // namespace

void storageInit() {
  if (!s_nvsMux) {
    s_nvsMux = xSemaphoreCreateMutex();
  }
  NvsLock lock;
  if (s_open) {
    return;
  }
  clearLegacyLgThermaNsOnce();
  ensureOpen();
}

bool storageLoadWifiEnabled() {
  NvsLock lock;
  ensureOpen();
  return s_prefs.getBool(kKeyWifiEn, false);
}

bool storageWifiEnabledIsSet() {
  NvsLock lock;
  ensureOpen();
  return s_prefs.isKey(kKeyWifiEn);
}

void storageSaveWifiEnabled(bool on) {
  NvsLock lock;
  ensureOpen();
  s_prefs.putBool(kKeyWifiEn, on);
}

bool storageLoadWifiCredentials(char* ssid, size_t ssidLen, char* pass, size_t passLen) {
  if (!ssid || ssidLen == 0 || !pass || passLen == 0) {
    return false;
  }
  NvsLock lock;
  ensureOpen();
  String storedSsid = s_prefs.getString(kKeyWifiSsid, "");
  String storedPass = s_prefs.getString(kKeyWifiPass, "");
  if (storedSsid.length() == 0) {
    ssid[0] = '\0';
    pass[0] = '\0';
    return false;
  }
  strncpy(ssid, storedSsid.c_str(), ssidLen - 1);
  ssid[ssidLen - 1] = '\0';
  strncpy(pass, storedPass.c_str(), passLen - 1);
  pass[passLen - 1] = '\0';
  return true;
}

void storageSaveWifiCredentials(const char* ssid, const char* pass) {
  if (!ssid || !pass) {
    return;
  }
  NvsLock lock;
  ensureOpen();
  s_prefs.putString(kKeyWifiSsid, ssid);
  s_prefs.putString(kKeyWifiPass, pass);
}

bool storageLoadMqttEnabled() {
  NvsLock lock;
  ensureOpen();
  return s_prefs.getBool(kKeyMqttEn, false);
}

void storageSaveMqttEnabled(bool on) {
  NvsLock lock;
  ensureOpen();
  s_prefs.putBool(kKeyMqttEn, on);
}

uint8_t storageLoadBrightness(void) {
  NvsLock lock;
  ensureOpen();
  if (!s_open) {
    return 60;
  }
  int v = 60;
  if (s_prefs.isKey(kKeyBlPct)) {
    v = s_prefs.getInt(kKeyBlPct, -1);
    if (v < 0) {
      v = static_cast<int>(s_prefs.getUInt(kKeyBlPct, 60));
    }
  }
  if (v < 10) {
    v = 10;
  }
  if (v > 97) {
    v = 97;
  }
  APP_SLOG("[NVS] load jas=%d\n", v);
  return static_cast<uint8_t>(v);
}

void storageSaveBrightness(uint8_t percent) {
  NvsLock lock;
  ensureOpen();
  if (!s_open) {
    return;
  }
  if (percent < 10) {
    percent = 10;
  }
  if (percent > 97) {
    percent = 97;
  }
  // TYPE_MISMATCH (starý typ klíče) → smazat a zapsat znovu
  size_t n = s_prefs.putInt(kKeyBlPct, percent);
  if (n == 0) {
    s_prefs.remove(kKeyBlPct);
    n = s_prefs.putInt(kKeyBlPct, percent);
  }
  APP_SLOG("[NVS] save jas=%u → %s\n", (unsigned)percent, n ? "ok" : "FAIL");
  if (!n) {
    Serial.printf("[NVS] save jas=%u → FAIL\n", (unsigned)percent);
  }
}

uint32_t storageLoadSleepTimeoutSec(void) {
  NvsLock lock;
  ensureOpen();
  if (!s_open) {
    return 120;
  }
  uint32_t sec = 120;
  if (s_prefs.isKey(kKeyBlSleep)) {
    sec = s_prefs.getUInt(kKeyBlSleep, UINT32_MAX);
    if (!isValidSleepTimeoutSec(sec)) {
      const int alt = s_prefs.getInt(kKeyBlSleep, -1);
      if (alt >= 0 && isValidSleepTimeoutSec(static_cast<uint32_t>(alt))) {
        sec = static_cast<uint32_t>(alt);
      } else {
        sec = 120;
      }
    }
  }
  APP_SLOG("[NVS] load usinani=%lus\n", (unsigned long)sec);
  return sec;
}

void storageSaveSleepTimeoutSec(uint32_t sec) {
  NvsLock lock;
  ensureOpen();
  if (!s_open) {
    return;
  }
  if (!isValidSleepTimeoutSec(sec)) {
    sec = 120;
  }
  size_t n = s_prefs.putUInt(kKeyBlSleep, sec);
  if (n == 0) {
    s_prefs.remove(kKeyBlSleep);
    n = s_prefs.putUInt(kKeyBlSleep, sec);
  }
  APP_SLOG("[NVS] save usinani=%lu → %s\n", (unsigned long)sec, n ? "ok" : "FAIL");
  if (!n) {
    Serial.printf("[NVS] save usinani=%lu → FAIL\n", (unsigned long)sec);
  }
}

bool storagePlanConfigKeyExists(void) {
  NvsLock lock;
  ensureOpen();
  if (!s_open) {
    return false;
  }
  return s_prefs.isKey(kKeyPlan);
}

void savePlanConfigLocked(const PlanTydenConfig* cfg);

bool storageLoadPlanConfig(PlanTydenConfig* cfg) {
  if (!cfg) {
    return false;
  }
  NvsLock lock;
  ensureOpen();
  if (!s_open) {
    Serial.println("[NVS] plan_cfg load: NVS not open");
    return false;
  }
  const size_t len = s_prefs.getBytesLength(kKeyPlan);
  // Dost místa i pro starší raw blob s paddingem; při větším blobu nenačítat (neřezat).
  constexpr size_t kMaxBlob = 6 + 192;
  if (len < 6 + kPlanPayloadV3) {
    Serial.printf("[NVS] plan_cfg load: chybi/zkraceny blob len=%u (min %u)\n",
                  (unsigned)len, (unsigned)(6 + kPlanPayloadV3));
    return false;
  }
  if (len > kMaxBlob) {
    Serial.printf("[NVS] plan_cfg load: blob moc velky len=%u\n", (unsigned)len);
    return false;
  }
  uint8_t buf[kMaxBlob];
  const size_t got = s_prefs.getBytes(kKeyPlan, buf, sizeof(buf));
  if (got != len || got < 6 + kPlanPayloadV3) {
    Serial.printf("[NVS] plan_cfg load: getBytes=%u expected=%u\n", (unsigned)got,
                  (unsigned)len);
    return false;
  }
  uint32_t magic = 0;
  uint16_t version = 0;
  memcpy(&magic, buf, 4);
  memcpy(&version, buf + 4, 2);
  if (magic != kPlanMagic) {
    Serial.printf("[NVS] plan_cfg load: bad magic 0x%08lX\n",
                  (unsigned long)magic);
    return false;
  }
  const size_t payloadLen = got - 6;
  if (!decodePlanPayload(buf + 6, payloadLen, version, cfg)) {
    Serial.printf("[NVS] plan_cfg load: decode FAIL ver=%u payload=%u ram=%u packed=%u\n",
                  (unsigned)version, (unsigned)payloadLen,
                  (unsigned)sizeof(PlanTydenConfig), (unsigned)kPlanPayloadPacked);
    return false;
  }
  // Nikdy nepřepisovat NVS při bootu — špatný decode + migrate dřív mazal plán po uploadu.
  // Upgrade na v5 proběhne až při climatePlanSave (odchod z UI / dirty flush).
  APP_SLOG("[NVS] plan_cfg load ok ver=%u payload=%u aktivni=%d\n",
                (unsigned)version, (unsigned)payloadLen, (int)cfg->aktivni);
  return true;
}

void savePlanConfigLocked(const PlanTydenConfig* cfg) {
  if (!cfg || !s_open) {
    return;
  }
  PlanTydenConfigStored packed{};
  planConfigToStored(cfg, &packed);
  uint8_t buf[6 + kPlanPayloadPacked];
  memcpy(buf, &kPlanMagic, 4);
  memcpy(buf + 4, &kPlanVersion, 2);
  memcpy(buf + 6, &packed, sizeof(packed));
  // remove před put — jisté přepsání i při změně délky blobu
  s_prefs.remove(kKeyPlan);
  s_prefs.putBytes(kKeyPlan, buf, sizeof(buf));
}

void storageSavePlanConfig(const PlanTydenConfig* cfg) {
  if (!cfg) {
    return;
  }
  NvsLock lock;
  ensureOpen();
  if (!s_open) {
    Serial.println("[NVS] plan_cfg save: NVS not open");
    return;
  }
  savePlanConfigLocked(cfg);
  const size_t checkLen = s_prefs.getBytesLength(kKeyPlan);
  const bool ok = checkLen == 6 + kPlanPayloadPacked;
  APP_SLOG("[NVS] plan_cfg save aktivni=%d bytes=%u → %s\n",
                (int)cfg->aktivni, (unsigned)checkLen, ok ? "ok" : "FAIL");
  if (!ok) {
    Serial.printf("[NVS] plan_cfg save FAIL bytes=%u\n", (unsigned)checkLen);
  }
}

void saveRegulatorConfigLocked(const RegulatorConfig* cfg) {
  if (!cfg || !s_open) {
    return;
  }
  uint8_t buf[6 + sizeof(RegulatorConfig)];
  memcpy(buf, &kRegMagic, 4);
  memcpy(buf + 4, &kRegVersion, 2);
  memcpy(buf + 6, cfg, sizeof(RegulatorConfig));
  s_prefs.remove(kKeyReg);
  s_prefs.putBytes(kKeyReg, buf, sizeof(buf));
}

bool storageRegulatorConfigKeyExists(void) {
  NvsLock lock;
  ensureOpen();
  if (!s_open) {
    return false;
  }
  return s_prefs.isKey(kKeyReg);
}

bool storageLoadRoomSpTenths(int16_t* outTenths) {
  if (!outTenths) {
    return false;
  }
  NvsLock lock;
  ensureOpen();
  if (!s_open || !s_prefs.isKey(kKeyRoomSpX10)) {
    return false;
  }
  const int v = s_prefs.getInt(kKeyRoomSpX10, -1);
  if (v < 180 || v > 240) {
    return false;
  }
  *outTenths = static_cast<int16_t>(v);
  return true;
}

void storageSaveRoomSpTenths(int16_t tenths) {
  if (tenths < 180) {
    tenths = 180;
  }
  if (tenths > 240) {
    tenths = 240;
  }
  NvsLock lock;
  ensureOpen();
  if (!s_open) {
    Serial.println("[NVS] room_sp_x10 save: NVS not open");
    return;
  }
  const size_t n = s_prefs.putInt(kKeyRoomSpX10, static_cast<int32_t>(tenths));
  if (!n) {
    Serial.printf("[NVS] room_sp_x10=%d → FAIL\n", (int)tenths);
  }
}

bool storageLoadRegulatorConfig(RegulatorConfig* cfg) {
  if (!cfg) {
    return false;
  }
  NvsLock lock;
  ensureOpen();
  size_t len = s_prefs.getBytesLength(kKeyReg);
  if (len < 6) {
    return false;
  }
  constexpr size_t kMaxBlob = 6 + 128;
  if (len > kMaxBlob) {
    Serial.printf("[NVS] reg_cfg blob moc velky len=%u\n", (unsigned)len);
    return false;
  }
  uint8_t buf[kMaxBlob];
  const size_t got = s_prefs.getBytes(kKeyReg, buf, sizeof(buf));
  if (got != len || got < 6) {
    return false;
  }
  uint32_t magic = 0;
  uint16_t version = 0;
  memcpy(&magic, buf, 4);
  memcpy(&version, buf + 4, 2);
  if (magic != kRegMagic || version < 1) {
    return false;
  }
  if (version >= kRegVersion) {
    if (got < 6 + sizeof(RegulatorConfig)) {
      Serial.printf("[NVS] reg_cfg v%u kratky got=%u need=%u\n", (unsigned)version,
                    (unsigned)got, (unsigned)(6 + sizeof(RegulatorConfig)));
      return false;
    }
    memcpy(cfg, buf + 6, sizeof(RegulatorConfig));
    return true;
  }
  if (version <= 4) {
    if (got < 6 + sizeof(RegulatorConfigV4)) {
      return false;
    }
    RegulatorConfigV4 old{};
    memcpy(&old, buf + 6, sizeof(RegulatorConfigV4));
    migrateRegulatorV4ToV5(&old, cfg);
    // Jen RAM — NVS přepíše climateRegulatorSave / room_sp_x10.
    return true;
  }
  return false;
}

void storageSaveRegulatorConfig(const RegulatorConfig* cfg) {
  if (!cfg) {
    return;
  }
  NvsLock lock;
  ensureOpen();
  saveRegulatorConfigLocked(cfg);
  const size_t checkLen = s_prefs.getBytesLength(kKeyReg);
  const bool ok = checkLen == 6 + sizeof(RegulatorConfig);
  APP_SLOG("[NVS] reg_cfg save room_sp=%.1f bytes=%u → %s\n",
                (double)cfg->room_sp_c, (unsigned)checkLen, ok ? "ok" : "FAIL");
  if (!ok) {
    Serial.printf("[NVS] reg_cfg save FAIL bytes=%u\n", (unsigned)checkLen);
  }
}

bool storageLoadUiRezim(uint8_t* out) {
  if (!out) {
    return false;
  }
  NvsLock lock;
  ensureOpen();
  if (!s_prefs.isKey(kKeyUiRezim)) {
    return false;
  }
  const int v = s_prefs.getInt(kKeyUiRezim, 0);
  if (v != 0 && v != 1 && v != 2) {
    return false;
  }
  *out = (uint8_t)v;
  return true;
}

void storageSaveUiRezim(uint8_t rezim) {
  NvsLock lock;
  ensureOpen();
  s_prefs.putInt(kKeyUiRezim, (int)rezim);
}

bool storageLoadTcSession(bool* outOn, uint8_t* outSp) {
  if (!outOn) {
    return false;
  }
  NvsLock lock;
  ensureOpen();
  if (!s_prefs.isKey(kKeyTcOn)) {
    return false;
  }
  *outOn = s_prefs.getBool(kKeyTcOn, false);
  if (outSp) {
    const int sp = s_prefs.getInt(kKeyTcSp, 35);
    if (sp >= 15 && sp <= 65) {
      *outSp = (uint8_t)sp;
    } else {
      *outSp = 35;
    }
  }
  return true;
}

void storageSaveTcSession(bool on, uint8_t spC) {
  NvsLock lock;
  ensureOpen();
  s_prefs.putBool(kKeyTcOn, on);
  if (spC >= 15 && spC <= 65) {
    s_prefs.putInt(kKeyTcSp, (int)spC);
  }
}

void storageRequestSaveTcSession(bool on, uint8_t spC) {
  s_tcSessionOnPending = on;
  if (spC >= 15 && spC <= 65) {
    s_tcSessionSpPending = spC;
  }
  s_tcSessionDirty = true;
}

void storageFlushTcSessionPending(void) {
  if (!s_tcSessionDirty) {
    return;
  }
  s_tcSessionDirty = false;
  storageSaveTcSession(s_tcSessionOnPending, s_tcSessionSpPending);
}

bool storageLoadBleRoomMac(char* mac, size_t len) {
  if (!mac || len < H2_MAC_STR_LEN) {
    return false;
  }
  NvsLock lock;
  ensureOpen();
  // isKey dřív než getString — jinak Preferences loguje ESP_LOGE při NOT_FOUND.
  if (!s_prefs.isKey(kKeyBleRoomMac)) {
    mac[0] = '\0';
    return false;
  }
  String s = s_prefs.getString(kKeyBleRoomMac, "");
  if (s.length() != 17) {
    mac[0] = '\0';
    return false;
  }
  strncpy(mac, s.c_str(), len - 1);
  mac[len - 1] = '\0';
  return true;
}

void storageSaveBleRoomMac(const char* mac) {
  NvsLock lock;
  ensureOpen();
  if (!mac || mac[0] == '\0' || strcmp(mac, "00:00:00:00:00:00") == 0) {
    s_prefs.remove(kKeyBleRoomMac);
    return;
  }
  s_prefs.putString(kKeyBleRoomMac, mac);
}

namespace {

const char* weekPowerKey(int day) {
  static const char* keys[] = {kKeyEnPwr0, kKeyEnPwr1, kKeyEnPwr2, kKeyEnPwr3,
                               kKeyEnPwr4, kKeyEnPwr5, kKeyEnPwr6};
  if (day < 0 || day > 6) {
    return kKeyEnPwr0;
  }
  return keys[day];
}

/** Volat pod NvsLock + ensureOpen. */
void storagePruneEnergyWeekPowerOldDaysUnlocked(void) {
  // en_p2..en_p6 = 5×2,8 kB — celé NVS má jen 20 kB.
  for (int d = 2; d < 7; ++d) {
    const char* key = weekPowerKey(d);
    if (s_prefs.isKey(key)) {
      s_prefs.remove(key);
      APP_SLOG("[NVS] pruned %s (free space for today graph)\n", key);
    }
  }
}

}  // namespace

bool storageEnergyMetaKeyExists(void) {
  NvsLock lock;
  ensureOpen();
  return s_prefs.isKey(kKeyEnMeta);
}

bool storageLoadEnergyMeta(void* dst, size_t len) {
  if (!dst || len == 0) {
    return false;
  }
  NvsLock lock;
  ensureOpen();
  if (!s_prefs.isKey(kKeyEnMeta)) {
    return false;
  }
  const size_t got = s_prefs.getBytesLength(kKeyEnMeta);
  // Prefix OK (větší blob z dočasného v3) i menší (starší FW) — zbytek vynuluj.
  if (got < sizeof(uint32_t) + sizeof(uint16_t)) {
    return false;
  }
  memset(dst, 0, len);
  const size_t n = (got < len) ? got : len;
  return s_prefs.getBytes(kKeyEnMeta, dst, n) == n;
}

void storageSaveEnergyMeta(const void* src, size_t len) {
  if (!src || len == 0) {
    return;
  }
  NvsLock lock;
  ensureOpen();
  if (s_prefs.putBytes(kKeyEnMeta, src, len) != len) {
    Serial.println("[NVS] en_meta putBytes FAIL");
  }
}

bool storageLoadEnergyWeekPower(uint16_t* dst, size_t count) {
  if (!dst || count < 7 * 1440) {
    return false;
  }
  NvsLock lock;
  ensureOpen();
  // Ne prune při load/boot — flash erase během Wi‑Fi.begin shazuje Tab5 SDIO.
  bool any = false;
  for (int d = 0; d < 7; ++d) {
    uint16_t* day = dst + d * 1440;
    if (d > 1) {
      memset(day, 0, 1440 * sizeof(uint16_t));
      continue;
    }
    const char* key = weekPowerKey(d);
    if (!s_prefs.isKey(key)) {
      memset(day, 0, 1440 * sizeof(uint16_t));
      continue;
    }
    const size_t want = 1440 * sizeof(uint16_t);
    if (s_prefs.getBytesLength(key) != want) {
      memset(day, 0, want);
      continue;
    }
    if (s_prefs.getBytes(key, day, want) == want) {
      any = true;
    } else {
      memset(day, 0, want);
    }
  }
  return any;
}

static bool putEnergyDay(int d, const uint16_t* daySamples) {
  if (d < 0 || d > 1) {
    // Do NVS jen dnes + včera.
    return true;
  }
  const char* key = weekPowerKey(d);
  const size_t want = 1440 * sizeof(uint16_t);
  s_prefs.remove(key);
  const size_t got = s_prefs.putBytes(key, daySamples, want);
  if (got != want) {
    Serial.printf("[NVS] %s putBytes FAIL (%u/%u) — NVS plné?\n", key,
                  (unsigned)got, (unsigned)want);
    return false;
  }
  return true;
}

void storageSaveEnergyWeekPower(const uint16_t* src, size_t count) {
  if (!src || count < 7 * 1440) {
    return;
  }
  NvsLock lock;
  ensureOpen();
  storagePruneEnergyWeekPowerOldDaysUnlocked();
  for (int d = 0; d <= 1; ++d) {
    if (!putEnergyDay(d, src + d * 1440)) {
      break;
    }
    delay(1);
  }
}

void storageSaveEnergyWeekPowerDay(int dayIndex, const uint16_t* daySamples) {
  if (!daySamples || dayIndex < 0 || dayIndex > 6) {
    return;
  }
  if (dayIndex > 1) {
    return;
  }
  NvsLock lock;
  ensureOpen();
  (void)putEnergyDay(dayIndex, daySamples);
}

void storagePruneEnergyWeekPowerOldDays(void) {
  NvsLock lock;
  ensureOpen();
  storagePruneEnergyWeekPowerOldDaysUnlocked();
}

void storageClearEnergyHistory(void) {
  NvsLock lock;
  ensureOpen();
  s_prefs.remove(kKeyEnMeta);
  for (int d = 0; d < 7; ++d) {
    s_prefs.remove(weekPowerKey(d));
  }
  APP_SLOG_LN("[NVS] energy history cleared");
}
