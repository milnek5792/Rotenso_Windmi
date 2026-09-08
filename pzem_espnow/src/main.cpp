// Waveshare ESP32-S3-Relay-1CH — PZEM-004T TTL (GPIO1 RX / GPIO2 TX) → ESP-NOW + OTA
#include <Arduino.h>
#include <ArduinoOTA.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <string.h>

#include "espnow_energy_config.h"
#include "pzem_ota_config.h"
#include "wifi_config.h"

#ifndef PZEM_RX_PIN
#define PZEM_RX_PIN 1
#endif
#ifndef PZEM_TX_PIN
#define PZEM_TX_PIN 2
#endif
#ifndef PZEM_UART_BAUD
#define PZEM_UART_BAUD 9600
#endif
#ifndef PZEM_ADDR
#define PZEM_ADDR 0xF8
#endif
#ifndef PZEM_ENERGY_RESET_WH
#define PZEM_ENERGY_RESET_WH 9000000u  // 9000 kWh
#endif

namespace {

HardwareSerial PzemSerial(1);

constexpr uint32_t kSampleMs = 1000;
constexpr uint32_t kSendMs = 60000;
constexpr uint32_t kPilotMs = 10000;
constexpr size_t kAvgSlots = 60;

uint16_t s_samples[kAvgSlots];
size_t s_sampleCount = 0;
uint32_t s_lastSampleMs = 0;
uint32_t s_lastSendMs = 0;
uint32_t s_lastPilotMs = 0;
uint8_t s_seq = 0;
uint8_t s_pilotSeq = 0;
bool s_resetPendingFlag = false;
bool s_espNowOk = false;
bool s_otaReady = false;
volatile bool s_otaBusy = false;
bool s_wifiConnected = false;
bool s_wifiConnecting = false;
uint32_t s_wifiConnectStartMs = 0;
uint32_t s_wifiRetryAtMs = 0;
uint8_t s_pzemOkCnt = 0;
uint8_t s_pzemFailCnt = 0;
uint32_t s_lastGoodEnergyWh = 0;
bool s_haveGoodEnergy = false;
bool s_lastSamplePzemOk = false;

uint8_t s_peerMac[6] = {
    ESPNOW_ENERGY_PEER_MAC0, ESPNOW_ENERGY_PEER_MAC1, ESPNOW_ENERGY_PEER_MAC2,
    ESPNOW_ENERGY_PEER_MAC3, ESPNOW_ENERGY_PEER_MAC4, ESPNOW_ENERGY_PEER_MAC5,
};

uint16_t modbusCrc(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int b = 0; b < 8; ++b) {
      if (crc & 1u) {
        crc = (uint16_t)((crc >> 1) ^ 0xA001u);
      } else {
        crc = (uint16_t)(crc >> 1);
      }
    }
  }
  return crc;
}

uint16_t be16(const uint8_t* p) {
  return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

/** PZEM-004T V3: 32bit = 1. reg (low) + 2. reg (high), každý BE. */
uint32_t pzemU32(const uint8_t* p) {
  return ((uint32_t)be16(p)) | ((uint32_t)be16(p + 2) << 16);
}

bool pzemTransact(const uint8_t* req, size_t reqLen, uint8_t* resp, size_t respLen,
                  uint32_t timeoutMs) {
  while (PzemSerial.available() > 0) {
    (void)PzemSerial.read();
  }
  PzemSerial.write(req, reqLen);
  PzemSerial.flush();

  size_t got = 0;
  const uint32_t start = millis();
  while (got < respLen && (millis() - start) < timeoutMs) {
    if (PzemSerial.available() > 0) {
      resp[got++] = (uint8_t)PzemSerial.read();
    } else {
      if (s_otaReady) {
        ArduinoOTA.handle();
      }
      delay(1);
    }
  }
  if (got < respLen) {
    Serial.printf("[PZEM] timeout got=%u/%u\n", (unsigned)got, (unsigned)respLen);
    return false;
  }
  const uint16_t crc = modbusCrc(resp, respLen - 2);
  const uint16_t gotCrc =
      (uint16_t)resp[respLen - 2] | ((uint16_t)resp[respLen - 1] << 8);
  if (crc != gotCrc) {
    Serial.printf("[PZEM] CRC fail calc=0x%04X got=0x%04X\n", crc, gotCrc);
    return false;
  }
  return true;
}

bool pzemReadPowerEnergy(float* outPowerW, uint32_t* outEnergyWh) {
  // Reg 0x0003..0x0006: Power (2) + Energy (2)
  uint8_t req[8] = {PZEM_ADDR, 0x04, 0x00, 0x03, 0x00, 0x04, 0x00, 0x00};
  const uint16_t crc = modbusCrc(req, 6);
  req[6] = (uint8_t)(crc & 0xFF);
  req[7] = (uint8_t)(crc >> 8);

  uint8_t resp[13];
  if (!pzemTransact(req, sizeof(req), resp, sizeof(resp), 200)) {
    return false;
  }
  if (resp[0] != PZEM_ADDR || resp[1] != 0x04 || resp[2] != 0x08) {
    Serial.printf("[PZEM] hdr fail %02X %02X %02X\n", resp[0], resp[1], resp[2]);
    return false;
  }

  // Power 0.1 W, Energy 1 Wh — low register first (ne pure BE32)
  const uint32_t powerRaw = pzemU32(&resp[3]);
  const uint32_t energyWh = pzemU32(&resp[7]);
  if (outPowerW) {
    *outPowerW = (float)powerRaw / 10.0f;
  }
  if (outEnergyWh) {
    *outEnergyWh = energyWh;
  }
  return true;
}

bool pzemResetEnergy(void) {
  uint8_t req[4] = {PZEM_ADDR, 0x42, 0x00, 0x00};
  const uint16_t crc = modbusCrc(req, 2);
  req[2] = (uint8_t)(crc & 0xFF);
  req[3] = (uint8_t)(crc >> 8);

  uint8_t resp[4];
  if (!pzemTransact(req, sizeof(req), resp, sizeof(resp), 300)) {
    return false;
  }
  return resp[0] == PZEM_ADDR && resp[1] == 0x42;
}

bool isBroadcastPeer(void) {
  for (int i = 0; i < 6; ++i) {
    if (s_peerMac[i] != 0xFF) {
      return false;
    }
  }
  return true;
}

uint8_t currentWifiChannel(void) {
  uint8_t primary = 0;
  wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
  if (esp_wifi_get_channel(&primary, &second) != ESP_OK || primary == 0) {
    return 0;
  }
  return primary;
}

void stopEspNow(void) {
  if (!s_espNowOk) {
    return;
  }
  esp_now_deinit();
  s_espNowOk = false;
  Serial.println("[ESPNOW] stopped");
}

bool initEspNow(void) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[ESPNOW] skip — WiFi offline (STA required)");
    s_espNowOk = false;
    return false;
  }

  stopEspNow();

  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESPNOW] init fail");
    s_espNowOk = false;
    return false;
  }

  if (esp_now_is_peer_exist(s_peerMac)) {
    esp_now_del_peer(s_peerMac);
  }

  esp_now_peer_info_t peer{};
  memcpy(peer.peer_addr, s_peerMac, 6);
  // 0 = kanál aktuální STA (stejný jako AP / Tab5 → C3 sync)
  peer.channel = 0;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("[ESPNOW] add peer fail");
    esp_now_deinit();
    s_espNowOk = false;
    return false;
  }

  s_espNowOk = true;
  Serial.printf("[ESPNOW] ok ch=%u peer=%02X:%02X:%02X:%02X:%02X:%02X%s\n",
                (unsigned)currentWifiChannel(), s_peerMac[0], s_peerMac[1],
                s_peerMac[2], s_peerMac[3], s_peerMac[4], s_peerMac[5],
                isBroadcastPeer() ? " (broadcast)" : "");
  return true;
}

void stopOta(void) {
  if (!s_otaReady) {
    return;
  }
  ArduinoOTA.end();
  s_otaReady = false;
  s_otaBusy = false;
  Serial.println("[OTA] stopped — WiFi offline");
}

void beginOta(void) {
  stopOta();
  ArduinoOTA.setHostname(PZEM_OTA_HOSTNAME);
  ArduinoOTA.setMdnsEnabled(true);
  if (PZEM_OTA_PASSWORD[0] != '\0') {
    ArduinoOTA.setPassword(PZEM_OTA_PASSWORD);
  }
  ArduinoOTA.onStart([]() {
    s_otaBusy = true;
    Serial.println("[OTA] start — pauza PZEM/ESP-NOW");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("[OTA] done — restart");
    delay(200);
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    static int s_lastPct = -1;
    if (total == 0) {
      return;
    }
    const int pct = (int)((progress * 100u) / total);
    if (pct != s_lastPct && (pct % 10) == 0) {
      s_lastPct = pct;
      Serial.printf("[OTA] %d%%\n", pct);
    }
  });
  ArduinoOTA.onError([](ota_error_t err) {
    s_otaBusy = false;
    Serial.printf("[OTA] error %u\n", (unsigned)err);
  });
  ArduinoOTA.begin();
  s_otaReady = true;
  Serial.printf("[OTA] ready host=%s.local ip=%s port=3232\n", PZEM_OTA_HOSTNAME,
                WiFi.localIP().toString().c_str());
  Serial.println("[OTA] pio: run -e esp32-s3-relay-pzem-ota -t upload");
}

bool wifiCredsOk(void) {
  return WIFI_SSID[0] != '\0' && strcmp(WIFI_SSID, "Vase_Sit") != 0;
}

void onWifiGot(void) {
  Serial.printf("[WIFI] ok ip=%s ch=%u rssi=%d\n",
                WiFi.localIP().toString().c_str(),
                (unsigned)currentWifiChannel(), (int)WiFi.RSSI());
  beginOta();
  if (!initEspNow()) {
    Serial.println("[ESPNOW] init after WiFi failed");
  }
}

void onWifiLost(void) {
  stopOta();
  stopEspNow();
  Serial.println("[WIFI] lost — OTA/ESP-NOW offline, reconnect...");
}

void startWifiConnect(void) {
  if (!wifiCredsOk()) {
    Serial.println("[WIFI] SSID not set in wifi_config.h — OTA/ESP-NOW unavailable");
    s_wifiConnecting = false;
    s_wifiRetryAtMs = millis() + PZEM_WIFI_RETRY_MS;
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(PZEM_OTA_HOSTNAME);
  WiFi.setSleep(false);
  WiFi.disconnect(false, false);
  delay(20);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  s_wifiConnecting = true;
  s_wifiConnectStartMs = millis();
  Serial.printf("[WIFI] connecting ssid=%s\n", WIFI_SSID);
}

void wifiTick(void) {
  if (!wifiCredsOk()) {
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (!s_wifiConnected) {
      s_wifiConnected = true;
      s_wifiConnecting = false;
      onWifiGot();
    } else if (!s_espNowOk) {
      (void)initEspNow();
    }
    return;
  }

  if (s_wifiConnected) {
    s_wifiConnected = false;
    onWifiLost();
    s_wifiRetryAtMs = millis() + PZEM_WIFI_RETRY_MS;
  }

  if (s_wifiConnecting) {
    if ((millis() - s_wifiConnectStartMs) >= PZEM_WIFI_CONNECT_MS) {
      s_wifiConnecting = false;
      WiFi.disconnect(false, false);
      s_wifiRetryAtMs = millis() + PZEM_WIFI_RETRY_MS;
      Serial.println(
          "[WIFI] connect timeout — retry; OTA/ESP-NOW nedostupné bez STA");
    }
    return;
  }

  if (s_wifiRetryAtMs == 0 || (int32_t)(millis() - s_wifiRetryAtMs) >= 0) {
    startWifiConnect();
  }
}

void pushSample(float powerW) {
  uint16_t w = 0;
  if (powerW > 0.0f) {
    if (powerW > 65535.0f) {
      w = 65535;
    } else {
      w = (uint16_t)(powerW + 0.5f);
    }
  }
  if (s_sampleCount < kAvgSlots) {
    s_samples[s_sampleCount++] = w;
  } else {
    memmove(&s_samples[0], &s_samples[1], (kAvgSlots - 1) * sizeof(uint16_t));
    s_samples[kAvgSlots - 1] = w;
  }
}

uint16_t averagePowerW(void) {
  if (s_sampleCount == 0) {
    return 0;
  }
  uint32_t sum = 0;
  for (size_t i = 0; i < s_sampleCount; ++i) {
    sum += s_samples[i];
  }
  return (uint16_t)(sum / (uint32_t)s_sampleCount);
}

void notePzemOk(uint32_t energyWh) {
  if (s_pzemOkCnt < 255) {
    ++s_pzemOkCnt;
  }
  s_lastGoodEnergyWh = energyWh;
  s_haveGoodEnergy = true;
  s_lastSamplePzemOk = true;
}

void notePzemFail(void) {
  if (s_pzemFailCnt < 255) {
    ++s_pzemFailCnt;
  }
  s_lastSamplePzemOk = false;
}

/** Pilot = důkaz rádia (nezávisle na tom, jestli PZEM odpovídá). */
void sendPilot(void) {
  if (!s_espNowOk) {
    return;
  }
  EspNowPilotPacket pkt{};
  pkt.magic = ESPNOW_PILOT_MAGIC;
  pkt.flags = s_lastSamplePzemOk ? ESPNOW_ENERGY_FLAG_PZEM_OK : 0;
  pkt.seq = ++s_pilotSeq;
  pkt.pzem_live = s_lastSamplePzemOk ? 1u : 0u;
  pkt._pad = 0;

  const esp_err_t err =
      esp_now_send(s_peerMac, reinterpret_cast<const uint8_t*>(&pkt), sizeof(pkt));
  Serial.printf("[ESPNOW] pilot seq=%u pzem=%u err=%d\n", (unsigned)pkt.seq,
                (unsigned)pkt.pzem_live, (int)err);
}

void sendMinute(uint32_t energyWh, bool pzemOk) {
  if (!s_espNowOk) {
    return;
  }
  // Nejdřív pilot — bridge pozná „rádio žije“, i kdyby PWR selhal/ztratil se
  sendPilot();
  s_lastPilotMs = millis();

  EspNowEnergyPacket pkt{};
  pkt.magic = ESPNOW_ENERGY_MAGIC;
  pkt.avg_power_w = averagePowerW();
  pkt.energy_wh = energyWh;
  pkt.flags = 0;
  if (s_resetPendingFlag) {
    pkt.flags |= ESPNOW_ENERGY_FLAG_RESET;
  }
  if (pzemOk) {
    pkt.flags |= ESPNOW_ENERGY_FLAG_PZEM_OK;
  }
  pkt.seq = ++s_seq;
  pkt.pzem_ok_cnt = s_pzemOkCnt;
  pkt.pzem_fail_cnt = s_pzemFailCnt;
  s_resetPendingFlag = false;

  const esp_err_t err =
      esp_now_send(s_peerMac, reinterpret_cast<const uint8_t*>(&pkt), sizeof(pkt));
  Serial.printf(
      "[ESPNOW] send W=%u E=%lu Wh flags=0x%02X seq=%u ok=%u fail=%u err=%d\n",
      (unsigned)pkt.avg_power_w, (unsigned long)pkt.energy_wh,
      (unsigned)pkt.flags, (unsigned)pkt.seq, (unsigned)pkt.pzem_ok_cnt,
      (unsigned)pkt.pzem_fail_cnt, (int)err);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("[PZEM] S3 Relay TTL meter boot");
  Serial.println("[PZEM] model: STA WiFi always (kanál AP + OTA + ESP-NOW)");

  PzemSerial.begin(PZEM_UART_BAUD, SERIAL_8N1, PZEM_RX_PIN, PZEM_TX_PIN);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(50);

  // Blokující první pokus — ať ESP-NOW/OTA jedou hned po bootu, pokud AP žije
  startWifiConnect();
  while (s_wifiConnecting &&
         (millis() - s_wifiConnectStartMs) < PZEM_WIFI_CONNECT_MS) {
    if (WiFi.status() == WL_CONNECTED) {
      break;
    }
    delay(200);
    Serial.print('.');
  }
  Serial.println();
  wifiTick();  // onWifiGot / timeout + schedule retry

  if (!s_wifiConnected) {
    Serial.println("[WIFI] boot: offline — ESP-NOW/OTA čekají na reconnect");
  }

  s_lastSampleMs = millis();
  s_lastSendMs = millis();
  s_lastPilotMs = 0;  // první pilot hned v loop (až bude WiFi)
}

void loop() {
  wifiTick();

  // OTA musí běžet často — i během dlouhého Modbus timeoutu
  if (s_otaReady) {
    ArduinoOTA.handle();
  }
  if (s_otaBusy) {
    delay(1);
    return;
  }

  const uint32_t now = millis();

  if ((now - s_lastSampleMs) >= kSampleMs) {
    s_lastSampleMs = now;
    float powerW = 0.0f;
    uint32_t energyWh = 0;
    if (pzemReadPowerEnergy(&powerW, &energyWh)) {
      Serial.printf("[PZEM] ok P=%.1f W  E=%lu Wh (%.3f kWh)\n", (double)powerW,
                    (unsigned long)energyWh, (double)energyWh / 1000.0);
      pushSample(powerW);
      notePzemOk(energyWh);
      if (energyWh >= PZEM_ENERGY_RESET_WH) {
        if (pzemResetEnergy()) {
          s_resetPendingFlag = true;
          Serial.printf("[PZEM] energy reset at %lu Wh\n",
                        (unsigned long)energyWh);
          energyWh = 0;
          s_lastGoodEnergyWh = 0;
        } else {
          Serial.println("[PZEM] energy reset FAILED");
        }
      }
    } else {
      notePzemFail();
      Serial.println("[PZEM] read fail");
    }
    if (s_otaReady) {
      ArduinoOTA.handle();
    }
  }

  if ((now - s_lastSendMs) >= kSendMs) {
    s_lastSendMs = now;
    float powerW = 0.0f;
    uint32_t energyWh = 0;
    bool pzemOk = pzemReadPowerEnergy(&powerW, &energyWh);
    if (pzemOk) {
      Serial.printf("[PZEM] minute P=%.1f W  E=%lu Wh\n", (double)powerW,
                    (unsigned long)energyWh);
      pushSample(powerW);
      notePzemOk(energyWh);
    } else {
      notePzemFail();
      energyWh = s_haveGoodEnergy ? s_lastGoodEnergyWh : 0;
      Serial.printf("[PZEM] minute read fail — send link diag E=%lu Wh\n",
                    (unsigned long)energyWh);
    }
    // Vždy poslat (i při výpadku měřiče) — bridge pozná signál vs. PZEM
    sendMinute(energyWh, pzemOk || s_pzemOkCnt > 0);
    s_sampleCount = 0;
    s_pzemOkCnt = 0;
    s_pzemFailCnt = 0;
    if (s_otaReady) {
      ArduinoOTA.handle();
    }
  } else if ((now - s_lastPilotMs) >= kPilotMs) {
    s_lastPilotMs = now;
    sendPilot();
    if (s_otaReady) {
      ArduinoOTA.handle();
    }
  }

  delay(5);
}
