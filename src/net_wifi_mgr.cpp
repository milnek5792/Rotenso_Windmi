#include "net_wifi_mgr.h"

#include "app_serial_trace.h"

#include "storage_config_nvs.h"
#include "wifi_config.h"

#include <Arduino.h>
#include <WiFi.h>
#include <string.h>

namespace {

enum class WifiPhase : uint8_t {
  kOff = 0,
  kIdle,
  kConnecting,
  kConnected,
  kFailed,
};

bool s_pinsReady = false;
bool s_enabled = false;
bool s_connected = false;
WifiPhase s_phase = WifiPhase::kOff;
unsigned long s_connectStartedMs = 0;

char s_status[40] = "Odpojeno";
char s_ssid[33] = "---";
char s_ip[16] = "---";
char s_credSsid[33] = "";
char s_credPass[65] = "";

constexpr unsigned long kConnectTimeoutMs = 30000;
constexpr unsigned long kRetryAfterFailMs = 15000;

unsigned long s_failStartedMs = 0;

void setStatus(const char* text) {
  strncpy(s_status, text ? text : "", sizeof(s_status));
  s_status[sizeof(s_status) - 1] = '\0';
}

void clearRuntimeNetworkInfo() {
  s_connected = false;
  strncpy(s_ssid, "---", sizeof(s_ssid));
  strncpy(s_ip, "---", sizeof(s_ip));
}

void ensureWifiPins() {
  if (s_pinsReady) { return; }

#if defined(BOARD_SDIO_ESP_HOSTED_CLK)
  WiFi.setPins(
      BOARD_SDIO_ESP_HOSTED_CLK, BOARD_SDIO_ESP_HOSTED_CMD, BOARD_SDIO_ESP_HOSTED_D0,
      BOARD_SDIO_ESP_HOSTED_D1, BOARD_SDIO_ESP_HOSTED_D2, BOARD_SDIO_ESP_HOSTED_D3,
      BOARD_SDIO_ESP_HOSTED_RESET);
#else
  WiFi.setPins(
      GPIO_NUM_12, GPIO_NUM_13, GPIO_NUM_11, GPIO_NUM_10,
      GPIO_NUM_9, GPIO_NUM_8, GPIO_NUM_15);
#endif

  s_pinsReady = true;
  APP_SLOG_LN("[NET] Wi-Fi SDIO piny nastaveny (Tab5 C6)");
}

bool compileTimeWifiOk() {
  return WIFI_SSID[0] != '\0' && strcmp(WIFI_SSID, "Vase_Sit") != 0 &&
         strcmp(WIFI_SSID, "YOUR_SSID") != 0;
}

bool loadCredentials() {
  // Nejdřív NVS (uložené z UI) — wifi_config.h jen jako fallback.
  if (storageLoadWifiCredentials(s_credSsid, sizeof(s_credSsid), s_credPass,
                                 sizeof(s_credPass))) {
    if (s_credSsid[0] != '\0') {
      return true;
    }
  }

  if (compileTimeWifiOk()) {
    strncpy(s_credSsid, WIFI_SSID, sizeof(s_credSsid) - 1);
    s_credSsid[sizeof(s_credSsid) - 1] = '\0';
    strncpy(s_credPass, WIFI_PASSWORD, sizeof(s_credPass) - 1);
    s_credPass[sizeof(s_credPass) - 1] = '\0';
    return true;
  }

  s_credSsid[0] = '\0';
  s_credPass[0] = '\0';
  return false;
}

void refreshConnectedState() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  s_connected = true;
  s_phase = WifiPhase::kConnected;
  setStatus("Připojeno");

  const String currentSsid = WiFi.SSID();
  if (currentSsid.length() > 0) {
    strncpy(s_ssid, currentSsid.c_str(), sizeof(s_ssid) - 1);
    s_ssid[sizeof(s_ssid) - 1] = '\0';
  }

  const String ip = WiFi.localIP().toString();
  if (ip.length() > 0) {
    strncpy(s_ip, ip.c_str(), sizeof(s_ip) - 1);
    s_ip[sizeof(s_ip) - 1] = '\0';
  }
  // NVS hesla neukládat tady — flash + SDIO Wi‑Fi zamrzá UI.
}

void startConnect() {
  if (!s_enabled) {
    setStatus("Zapnete Wi-Fi");
    return;
  }
  if (!loadCredentials()) {
    setStatus("Nastavte sit v menu");
    Serial.println("[NET] Wi-Fi: nastavte SSID v Nastaveni -> Nastavit sit");
    s_phase = WifiPhase::kFailed;
    return;
  }

  ensureWifiPins();
  // Tab5 C6 (esp_hosted): disconnect(wifioff=true) shodí SDIO a begin() pak
  // hlásí esp_hosted_init failed / STA enable failed.
  if (WiFi.getMode() != WIFI_STA) {
    WiFi.mode(WIFI_STA);
    delay(50);
  }
  WiFi.disconnect(false, false);
  delay(20);

  setStatus("Připojování...");
  strncpy(s_ssid, s_credSsid, sizeof(s_ssid) - 1);
  s_ssid[sizeof(s_ssid) - 1] = '\0';
  strncpy(s_ip, "---", sizeof(s_ip));

  APP_SLOG("[NET] Wi-Fi pripojuji k '%s'\n", s_credSsid);
  const bool ok = WiFi.begin(s_credSsid, s_credPass);
  if (!ok) {
    Serial.println("[NET] Wi-Fi begin() FAIL (esp_hosted?)");
    s_phase = WifiPhase::kFailed;
    s_failStartedMs = millis();
    setStatus("Wi-Fi init fail");
    return;
  }

  s_phase = WifiPhase::kConnecting;
  s_connectStartedMs = millis();
  s_connected = false;
}

}  // namespace

void netWifiInit() {
  storageInit();
  ensureWifiPins();

  const bool hasCompile = compileTimeWifiOk();

  // Po migraci NVS často zůstalo wifi_en=0 — s wifi_config.h vždy zapnout.
  if (hasCompile) {
    s_enabled = true;
    APP_SLOG_LN("[NET] Wi-Fi ON (wifi_config.h)");
  } else if (!storageWifiEnabledIsSet()) {
    s_enabled = false;
  } else {
    s_enabled = storageLoadWifiEnabled();
  }

  loadCredentials();

  // NVS seed jen když SSID v NVS chybí — ne při každém bootu (flash freeze).
  if (s_enabled && hasCompile) {
    char nvsSsid[33] = "";
    char nvsPass[65] = "";
    if (!storageLoadWifiCredentials(nvsSsid, sizeof(nvsSsid), nvsPass,
                                    sizeof(nvsPass))) {
      storageSaveWifiCredentials(s_credSsid, s_credPass);
      APP_SLOG_LN("[NET] Wi-Fi hesla ulozena do NVS (seed)");
    }
  }

  if (s_enabled) {
    WiFi.mode(WIFI_STA);
    delay(80);
    setStatus("Pripraveno");
    s_phase = WifiPhase::kIdle;

    if (WiFi.status() == WL_CONNECTED) {
      refreshConnectedState();
    } else if (netWifiHasCredentials()) {
      startConnect();
    }
  } else {
    WiFi.mode(WIFI_OFF);
    setStatus("Vypnuto");
    s_phase = WifiPhase::kOff;
    clearRuntimeNetworkInfo();
  }

  // wifi_en neukládat při každém bootu — flash + SDIO Wi‑Fi.
  if (!storageWifiEnabledIsSet()) {
    storageSaveWifiEnabled(s_enabled);
  }
}

void netWifiSetEnabled(bool on) {
  s_enabled = on;
  storageSaveWifiEnabled(on);

  if (!on) {
    ensureWifiPins();
    WiFi.disconnect(true, false);
    WiFi.mode(WIFI_OFF);
    s_phase = WifiPhase::kOff;
    setStatus("Vypnuto");
    clearRuntimeNetworkInfo();
    return;
  }

  ensureWifiPins();
  WiFi.mode(WIFI_STA);
  setStatus("Pripraveno");
  s_phase = WifiPhase::kIdle;
}

bool netWifiIsEnabled() {
  return s_enabled;
}

void netWifiConnect() {
  if (!s_enabled) {
    netWifiSetEnabled(true);
  }
  startConnect();
}

bool netWifiIsConnected() {
  return s_connected && WiFi.status() == WL_CONNECTED;
}

bool netWifiIsSuspendedForBle(void) { return false; }
bool netWifiSuspendForBle(void) { return true; }
bool netWifiResumeAfterBle(void) { return true; }

bool netWifiHasCredentials() {
  return loadCredentials();
}

bool netWifiCopyCredentials(char* ssid, size_t ssidLen, char* pass,
                            size_t passLen) {
  if (!loadCredentials()) {
    return false;
  }
  if (ssid && ssidLen > 0) {
    strncpy(ssid, s_credSsid, ssidLen - 1);
    ssid[ssidLen - 1] = '\0';
  }
  if (pass && passLen > 0) {
    strncpy(pass, s_credPass, passLen - 1);
    pass[passLen - 1] = '\0';
  }
  return s_credSsid[0] != '\0';
}

void netWifiSetCredentials(const char* ssid, const char* pass) {
  if (!ssid || !pass) { return; }
  strncpy(s_credSsid, ssid, sizeof(s_credSsid) - 1);
  s_credSsid[sizeof(s_credSsid) - 1] = '\0';
  strncpy(s_credPass, pass, sizeof(s_credPass) - 1);
  s_credPass[sizeof(s_credPass) - 1] = '\0';
  storageSaveWifiCredentials(s_credSsid, s_credPass);
}

const char* netWifiStatus() {
  return s_status;
}

const char* netWifiSsid() {
  return s_ssid;
}

const char* netWifiIp() {
  return s_ip;
}

uint8_t netWifiChannel(void) {
  if (!netWifiIsConnected()) {
    return 0;
  }
  const int ch = WiFi.channel();
  if (ch < 1 || ch > 14) {
    return 0;
  }
  return (uint8_t)ch;
}

void netWifiTick() {
  if (!s_enabled) { return; }

  if (WiFi.status() == WL_CONNECTED) {
    if (!s_connected || s_phase != WifiPhase::kConnected) {
      refreshConnectedState();
      Serial.printf("[NET] Wi-Fi OK: %s / %s\n", s_ssid, s_ip);
    }
    return;
  }

  // Po failu zkus znovu — jinak zůstane wifi=0 navždy.
  if (s_phase == WifiPhase::kFailed || s_phase == WifiPhase::kIdle) {
    if (s_phase == WifiPhase::kFailed &&
        (millis() - s_failStartedMs) < kRetryAfterFailMs) {
      return;
    }
    if (netWifiHasCredentials()) {
      startConnect();
    }
    return;
  }

  if (s_phase != WifiPhase::kConnecting) { return; }

  if (millis() - s_connectStartedMs >= kConnectTimeoutMs) {
    s_phase = WifiPhase::kFailed;
    s_failStartedMs = millis();
    s_connected = false;
    setStatus("Timeout připojení");
    Serial.println("[NET] Wi-Fi timeout");
    WiFi.disconnect(false, false);
    return;
  }

  wl_status_t st = WiFi.status();
  if (st == WL_NO_SSID_AVAIL) {
    s_phase = WifiPhase::kFailed;
    s_failStartedMs = millis();
    setStatus("Sit nenalezena");
    Serial.println("[NET] Wi-Fi: SSID nenalezeno");
  } else if (st == WL_CONNECT_FAILED) {
    s_phase = WifiPhase::kFailed;
    s_failStartedMs = millis();
    setStatus("Spatne heslo");
    Serial.println("[NET] Wi-Fi: pripojeni selhalo");
  }
}

bool netWifiIsBusy(void) {
  return s_phase == WifiPhase::kConnecting;
}
