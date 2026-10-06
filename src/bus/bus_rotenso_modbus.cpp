// bus_rotenso_modbus.cpp — Windmi Modbus RTU (pevné FC 0x04 read / 0x06 write)
// MVP: 1 TX/tick — TEMP (0001..0004) ↔ MODE (002C..002D); zápisy mají prioritu.
#include "src/bus/bus_rotenso_modbus.h"

#include "app_serial_trace.h"
#include "bus_rotenso_config.h"
#include "src/bus_lg_model.h"

#include <Arduino.h>
#include <cstring>
#include <math.h>

namespace {

#if defined(Serial1)
#define MbSerial Serial1
#else
HardwareSerial MbSerial(WINDMI_UART_NUM);
#endif

bool s_ready = false;
uint32_t s_lastTxMs = 0;
unsigned long s_ok = 0;
unsigned long s_fail = 0;
uint32_t s_lastOkMs = 0;
uint32_t s_writeNextMs = 0;  // backoff po fail zápisu — ať běží čtení

uint16_t s_setting = 0;
uint16_t s_running = 0;
int16_t s_compX10 = 0;
uint16_t s_pump = 0;
uint16_t s_quiet = 0;
uint16_t s_flow = 0;
uint16_t s_load = 0;
bool s_loadOk = false;
/** Stejná rychlost: teploty + kontrolky + průtok. SP občas. */
enum Step : uint8_t {
  STEP_TEMPS = 0,
  STEP_MODE,
  STEP_LOAD,
  STEP_COMP,
  STEP_PUMP,
  STEP_FLOW,
  STEP_QUIET,
  STEP_ALARM,
  STEP_COUNT,
};
uint8_t s_step = STEP_TEMPS;
uint8_t s_cycle = 0;
bool s_doSp = false;

constexpr uint8_t kCfgQCap = 8;
struct CfgWr {
  uint16_t addr;
  uint16_t value;
};
CfgWr s_cfgQ[kCfgQCap];
uint8_t s_cfgQHead = 0;
uint8_t s_cfgQTail = 0;
uint8_t s_cfgQCount = 0;

/** Obrazovka konfigurace TČ — číst RW registry (jinak MVP jen live). */
bool s_cfgScreen = false;
bool s_cfgNeedPoll = false;
uint8_t s_cfgStep = 0;
WindmiHpConfigSnap s_cfgAcc = {};

enum CfgStep : uint8_t {
  CFG_CTRL = 0,
  CFG_UI_TYPE,
  CFG_CURVE,
  CFG_BACKUP,
  CFG_MIN_OAT,
  CFG_IBH_WARM,
  CFG_IBH_DT,
  CFG_IBH_OAT,
  CFG_PUMP_DT,
  CFG_REQ_FREQ,
  CFG_COUNT,
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

void dirTx(bool transmit) {
  digitalWrite(WINDMI_RS485_DIR_PIN, transmit ? HIGH : LOW);
}

void drainRx() {
  while (MbSerial.available() > 0) {
    (void)MbSerial.read();
  }
}

void gapBeforeTx() {
  if (s_lastTxMs == 0) {
    return;
  }
  const uint32_t elapsed = millis() - s_lastTxMs;
  if (elapsed < WINDMI_MB_GAP_MS) {
    delay(WINDMI_MB_GAP_MS - elapsed);
  }
}

bool mbTransact(const uint8_t* req, size_t reqLen, uint8_t* resp, size_t respCap,
                size_t* gotOut) {
  gapBeforeTx();
  drainRx();

  dirTx(true);
  delayMicroseconds(50);
  MbSerial.write(req, reqLen);
  MbSerial.flush();
  delayMicroseconds(WINDMI_MB_TX_SETTLE_US);
  dirTx(false);

  size_t got = 0;
  const uint32_t start = millis();
  while ((millis() - start) < WINDMI_MB_TIMEOUT_MS) {
    if (MbSerial.available() > 0) {
      if (got < respCap) {
        resp[got++] = (uint8_t)MbSerial.read();
      } else {
        (void)MbSerial.read();
      }
      if (got >= 2) {
        const uint8_t fc = resp[1];
        size_t need = 0;
        if (fc & 0x80u) {
          // Exception: addr FC|0x80 exCode CRC CRC
          need = 5;
        } else if (fc == 0x06u || fc == 0x05u) {
          // Write single: echo 8 B (ne byte-count jako u 0x03/0x04!)
          need = 8;
        } else if (fc == 0x10u || fc == 0x0Fu) {
          need = 8;
        } else if (got >= 3) {
          // Read: addr FC byteCount data… CRC
          need = (size_t)3u + (size_t)resp[2] + 2u;
        }
        if (need != 0 && got >= need) {
          break;
        }
      }
    } else {
      delay(1);
    }
  }

  s_lastTxMs = millis();
  *gotOut = got;
  return got >= 5;
}

void noteFail(const char* why) {
  ++s_fail;
  if (s_fail <= 10u || (s_fail % 25u) == 0u) {
    Serial.printf("[WM] FAIL %s ok=%lu fail=%lu\n", why, s_ok, s_fail);
  }
}

void noteOk() {
  ++s_ok;
  s_lastOkMs = millis();
}

/** Čtení registrů (FC 0x04 input / 0x03 holding). Soft=true ⇒ nepočítat fail streak. */
bool mbReadRegs(uint8_t fc, uint16_t addr, uint8_t qty, uint8_t* dataOut,
                bool soft) {
  if (!dataOut || qty == 0 || qty > 16) {
    return false;
  }

  uint8_t req[8];
  req[0] = (uint8_t)WINDMI_MB_SLAVE;
  req[1] = fc;
  req[2] = (uint8_t)((addr >> 8) & 0xFF);
  req[3] = (uint8_t)(addr & 0xFF);
  req[4] = 0x00;
  req[5] = qty;
  const uint16_t crc = modbusCrc(req, 6);
  req[6] = (uint8_t)(crc & 0xFF);
  req[7] = (uint8_t)((crc >> 8) & 0xFF);

  const size_t expectPayload = (size_t)qty * 2u;
  const size_t expectLen = 3u + expectPayload + 2u;
  uint8_t resp[48];
  size_t got = 0;
  const unsigned long failBefore = s_fail;
  if (!mbTransact(req, sizeof(req), resp, sizeof(resp), &got)) {
    char why[32];
    snprintf(why, sizeof(why), "to@%04X fc%02X n=%u", (unsigned)addr,
             (unsigned)fc, (unsigned)got);
    noteFail(why);
    if (soft && s_fail > failBefore) {
      --s_fail;
    }
    return false;
  }
  if (resp[0] != (uint8_t)WINDMI_MB_SLAVE || (resp[1] & 0x80u) ||
      resp[1] != fc || got < expectLen ||
      resp[2] != (uint8_t)expectPayload) {
    char why[36];
    snprintf(why, sizeof(why), "bad@%04X n=%u fc=%02X", (unsigned)addr,
             (unsigned)got, got >= 2 ? (unsigned)resp[1] : 0u);
    noteFail(why);
    if (soft && s_fail > failBefore) {
      --s_fail;
    }
    return false;
  }
  const uint16_t calc = modbusCrc(resp, got - 2);
  const uint16_t gotCrc =
      (uint16_t)resp[got - 2] | ((uint16_t)resp[got - 1] << 8);
  if (calc != gotCrc) {
    noteFail("crc");
    if (soft && s_fail > failBefore) {
      --s_fail;
    }
    return false;
  }
  memcpy(dataOut, &resp[3], expectPayload);
  noteOk();
  return true;
}

/** Čtení Input Registers — FC 0x04. */
bool mbReadInput(uint16_t addr, uint8_t qty, uint8_t* dataOut) {
  return mbReadRegs((uint8_t)WINDMI_FC_READ, addr, qty, dataOut, false);
}

/** Zápis — vždy FC 0x06. */
bool mbWriteSingle(uint16_t addr, uint16_t value) {
  uint8_t req[8];
  req[0] = (uint8_t)WINDMI_MB_SLAVE;
  req[1] = (uint8_t)WINDMI_FC_WRITE;
  req[2] = (uint8_t)((addr >> 8) & 0xFF);
  req[3] = (uint8_t)(addr & 0xFF);
  req[4] = (uint8_t)((value >> 8) & 0xFF);
  req[5] = (uint8_t)(value & 0xFF);
  const uint16_t crc = modbusCrc(req, 6);
  req[6] = (uint8_t)(crc & 0xFF);
  req[7] = (uint8_t)((crc >> 8) & 0xFF);

  uint8_t resp[16];
  size_t got = 0;
  if (!mbTransact(req, sizeof(req), resp, sizeof(resp), &got)) {
    char why[24];
    snprintf(why, sizeof(why), "wr-to n=%u", (unsigned)got);
    noteFail(why);
    return false;
  }
  if (resp[0] != (uint8_t)WINDMI_MB_SLAVE || (resp[1] & 0x80u) || got < 8 ||
      resp[1] != (uint8_t)WINDMI_FC_WRITE) {
    Serial.printf("[WM] wr-bad n=%u %02X %02X %02X %02X %02X\n", (unsigned)got,
                  got > 0 ? resp[0] : 0, got > 1 ? resp[1] : 0,
                  got > 2 ? resp[2] : 0, got > 3 ? resp[3] : 0,
                  got > 4 ? resp[4] : 0);
    noteFail("wr-bad");
    return false;
  }
  const uint16_t calc = modbusCrc(resp, 6);
  const uint16_t gotCrc = (uint16_t)resp[6] | ((uint16_t)resp[7] << 8);
  if (calc != gotCrc || resp[2] != req[2] || resp[3] != req[3] ||
      resp[4] != req[4] || resp[5] != req[5]) {
    noteFail("wr-crc");
    return false;
  }
  noteOk();
  APP_SLOG_LN("[WM] WR OK");
  return true;
}

uint16_t be16(const uint8_t* p) {
  return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

bool decodeTemp(const uint8_t* p, float* outC) {
  const int16_t raw = (int16_t)be16(p);
  if (raw == (int16_t)WINDMI_TEMP_INVALID_RAW || raw == -40) {
    return false;
  }
  float c = (float)raw * WINDMI_TEMP_SCALE;
  if (raw >= 5 && raw <= 90 && c < 5.0f) {
    c = (float)raw;
  }
  if (c <= (WINDMI_TEMP_INVALID_C + 0.05f) || c > 120.0f) {
    return false;
  }
  *outC = c;
  return true;
}

void publishStatus() {
  lgModelSetMbStatus(s_setting, s_running, s_compX10, s_pump, s_quiet, s_load,
                     s_loadOk, s_flow);
}

bool cfgQPush(uint16_t addr, uint16_t value) {
  if (s_cfgQCount >= kCfgQCap) {
    return false;
  }
  s_cfgQ[s_cfgQTail].addr = addr;
  s_cfgQ[s_cfgQTail].value = value;
  s_cfgQTail = (uint8_t)((s_cfgQTail + 1u) % kCfgQCap);
  ++s_cfgQCount;
  return true;
}

bool cfgQPop(CfgWr* out) {
  if (s_cfgQCount == 0 || !out) {
    return false;
  }
  *out = s_cfgQ[s_cfgQHead];
  s_cfgQHead = (uint8_t)((s_cfgQHead + 1u) % kCfgQCap);
  --s_cfgQCount;
  return true;
}

bool processOneWrite() {
  // Zápisy až po alespoň 1 úspěšném čtení — jinak start-fail zablokuje bus
  if (s_ok == 0) {
    return false;
  }
  if (millis() < s_writeNextMs) {
    return false;
  }

  {
    lgModelLock();
    const bool want = pozadavekNaZapis && pozadavekZmenaStartu;
    const bool wantOn = cilovyZapnutoTab5 || tcPozadavekZap;
    const uint8_t spC = novaCilovaTeplota;
    lgModelUnlock();
    if (want) {
      const uint16_t mode =
          wantOn ? (uint16_t)WINDMI_SET_HEAT : (uint16_t)WINDMI_SET_OFF;
      APP_SLOG("[WM] WR MODE %u\n", (unsigned)mode);
      if (!mbWriteSingle((uint16_t)WINDMI_REG_SETTING_MODE, mode)) {
        s_writeNextMs = millis() + 500u;
        return true;
      }
      s_writeNextMs = 0;
      lgModelLock();
      if (pozadavekNaZapis && pozadavekZmenaStartu) {
        pozadavekZmenaStartu = false;
        pozadavekNaZapis = (wantOn && spC != 0);
      }
      mMbPowerPending = true;
      mMbPowerWantOn = wantOn;
      lgModelUnlock();
      return true;
    }
  }

  {
    lgModelLock();
    const bool want =
        pozadavekNaZapis && !pozadavekZmenaStartu && novaCilovaTeplota != 0;
    const uint8_t spC = novaCilovaTeplota;
    lgModelUnlock();
    if (want) {
      uint8_t clamped = spC;
      if (clamped < (uint8_t)WINDMI_WATER_SP_MIN_C) {
        clamped = (uint8_t)WINDMI_WATER_SP_MIN_C;
      }
      if (clamped > (uint8_t)WINDMI_WATER_SP_MAX_C) {
        clamped = (uint8_t)WINDMI_WATER_SP_MAX_C;
      }
      const uint16_t raw = (uint16_t)clamped * 10u;
      APP_SLOG("[WM] WR SP raw=%u\n", (unsigned)raw);
      if (!mbWriteSingle((uint16_t)WINDMI_REG_WATER_SP, raw)) {
        s_writeNextMs = millis() + 500u;
        return true;
      }
      s_writeNextMs = 0;
      lgModelLock();
      if (pozadavekNaZapis && !pozadavekZmenaStartu &&
          novaCilovaTeplota == spC) {
        pozadavekNaZapis = false;
      }
      mCilova = clamped;
      lgModelUnlock();
      lgModelSetMbWaterSp(clamped);
      return true;
    }
  }

  {
    CfgWr wr;
    if (cfgQPop(&wr)) {
      if (!mbWriteSingle(wr.addr, wr.value)) {
        s_writeNextMs = millis() + 2000u;
        cfgQPush(wr.addr, wr.value);  // zkus znovu později
      } else {
        s_writeNextMs = 0;
        s_cfgNeedPoll = true;  // po zápisu znovu načíst hodnoty z TČ
        s_cfgStep = 0;
      }
      return true;
    }
  }

  return false;
}

void runTemps() {
  uint8_t raw[8];
  if (!mbReadInput((uint16_t)WINDMI_REG_TEMP_BASE, 4, raw)) {
    return;
  }
  float outdoor = NAN, inlet = NAN, outlet = NAN;
  const bool outOk = decodeTemp(&raw[0], &outdoor);
  const bool inOk = decodeTemp(&raw[4], &inlet);
  const bool outwOk = decodeTemp(&raw[6], &outlet);
  if (inOk || outwOk || outOk) {
    lgModelSetMbTemps(outdoor, outOk, inlet, inOk, outlet, outwOk);
  } else {
    lgModelTouchLive();
  }
#if APP_SERIAL_TRACE
  char oS[12], iS[12], wS[12];
  if (outOk) {
    snprintf(oS, sizeof(oS), "%.1f", (double)outdoor);
  } else {
    snprintf(oS, sizeof(oS), "n/a");
  }
  if (inOk) {
    snprintf(iS, sizeof(iS), "%.1f", (double)inlet);
  } else {
    snprintf(iS, sizeof(iS), "n/a");
  }
  if (outwOk) {
    snprintf(wS, sizeof(wS), "%.1f", (double)outlet);
  } else {
    snprintf(wS, sizeof(wS), "n/a");
  }
  APP_SLOG("[WM] TEMP out=%s in=%s tw=%s\n", oS, iS, wS);
#endif
}

void runMode() {
  uint8_t buf[4];
  if (!mbReadInput((uint16_t)WINDMI_REG_SETTING_MODE, 2, buf)) {
    return;
  }
  s_setting = be16(&buf[0]);
  s_running = be16(&buf[2]);
  publishStatus();
  APP_SLOG("[WM] MODE set=%u run=%u\n", (unsigned)s_setting,
           (unsigned)s_running);
}

void runLoad() {
  uint8_t buf[2];
  // 0081 může vracet exception (fc=0x84) — nekazit live
  const unsigned long failBefore = s_fail;
  if (!mbReadInput((uint16_t)WINDMI_REG_LOAD_OUTPUT, 1, buf)) {
    if (s_fail > failBefore) {
      --s_fail;  // nepočítat optional reg do fail streak
    }
    return;
  }
  s_load = be16(buf);
  s_loadOk = true;
  publishStatus();
}

void runComp() {
  uint8_t buf[2];
  if (!mbReadInput((uint16_t)WINDMI_REG_COMP_FREQ, 1, buf)) {
    return;
  }
  s_compX10 = (int16_t)be16(buf);
  publishStatus();
}

void runPump() {
  uint8_t buf[2];
  if (!mbReadInput((uint16_t)WINDMI_REG_PUMP_SPEED, 1, buf)) {
    return;
  }
  s_pump = be16(buf);
  publishStatus();
}

void runFlow() {
  uint8_t buf[2];
  // 102AH Waterflow feedback (m³/h ×100) — soft fail jako LOAD
  if (!mbReadRegs((uint8_t)WINDMI_FC_READ, (uint16_t)WINDMI_REG_WATER_FLOW, 1,
                  buf, true)) {
    return;
  }
  s_flow = be16(buf);
  publishStatus();
  APP_SLOG("[WM] FLOW raw=%u (%.2f m3/h)\n", (unsigned)s_flow,
           (double)s_flow * 0.01);
}

void runQuiet() {
  uint8_t buf[2];
  if (!mbReadInput((uint16_t)WINDMI_REG_QUIET_NIGHT, 1, buf)) {
    return;
  }
  s_quiet = be16(buf);
  publishStatus();
  APP_SLOG("[WM] LED load=0x%04X comp=%d pump=%u flow=%u quiet=%u\n",
           (unsigned)s_load, (int)s_compX10, (unsigned)s_pump,
           (unsigned)s_flow, (unsigned)s_quiet);
}

void runAlarm() {
  uint8_t buf[8];
  if (!mbReadInput((uint16_t)WINDMI_REG_ALARM_BM1, (uint8_t)WINDMI_REG_ALARM_COUNT,
                   buf)) {
    return;
  }
  uint16_t bm[4];
  for (int i = 0; i < 4; ++i) {
    bm[i] = be16(buf + (size_t)i * 2u);
  }
  lgModelSetMbAlarms(bm);
  if ((bm[0] | bm[1] | bm[2] | bm[3]) != 0u) {
    Serial.printf("[WM] ALARM bm=%04X %04X %04X %04X\n", (unsigned)bm[0],
                  (unsigned)bm[1], (unsigned)bm[2], (unsigned)bm[3]);
  }
}

void runSp() {
  uint8_t raw[2];
  if (!mbReadInput((uint16_t)WINDMI_REG_WATER_SP, 1, raw)) {
    return;
  }
  const int16_t rawT = (int16_t)be16(raw);
  float c;
  if (rawT >= 150 && rawT <= 650) {
    c = (float)rawT * WINDMI_TEMP_SCALE;
  } else if (rawT >= 15 && rawT <= 65) {
    c = (float)rawT;
  } else {
    return;
  }
  const uint8_t sp = (uint8_t)(c + 0.5f);
  lgModelSetMbWaterSp(sp);
  APP_SLOG("[WM] SP %u C\n", (unsigned)sp);
}

bool readCfgRegSoft(uint16_t addr, uint16_t* out) {
  uint8_t buf[2];
  const unsigned long failBefore = s_fail;
  if (!mbReadInput(addr, 1, buf)) {
    if (s_fail > failBefore) {
      --s_fail;  // optional cfg reg — nekazit live streak
    }
    return false;
  }
  *out = be16(buf);
  return true;
}

/** 1 TX — další konfigurační registr; true = tick spotřebován. */
bool runConfigStep() {
  if (!s_cfgNeedPoll) {
    return false;
  }

  // Nepřepisovat UI nulami: start od posledního snapu, měnit jen úspěšné regy.
  if (s_cfgStep == 0) {
    lgModelReadHpConfigSnap(&s_cfgAcc);
  }

  uint16_t raw = 0;
  bool changed = false;
  switch (s_cfgStep) {
    case CFG_CTRL:
      if (readCfgRegSoft((uint16_t)WINDMI_REG_CTRL_MODE, &raw)) {
        if (!(s_cfgAcc.mask & 0x01u) || s_cfgAcc.ctrl_mode != raw) {
          changed = true;
        }
        s_cfgAcc.ctrl_mode = raw;
        s_cfgAcc.mask |= 0x01u;
        APP_SLOG("[WM] CFG 100D ctrl=%u\n", (unsigned)raw);
      }
      break;
    case CFG_UI_TYPE:
      if (readCfgRegSoft((uint16_t)WINDMI_REG_UI_TYPE, &raw)) {
        if (!(s_cfgAcc.mask & 0x0100u) || s_cfgAcc.ui_type != raw) {
          changed = true;
        }
        s_cfgAcc.ui_type = raw;
        s_cfgAcc.mask |= 0x0100u;
        APP_SLOG("[WM] CFG 0209 ui=%u\n", (unsigned)raw);
      }
      break;
    case CFG_CURVE:
      if (readCfgRegSoft((uint16_t)WINDMI_REG_CURVE_TYPE, &raw)) {
        const int16_t v = (int16_t)raw;
        if (!(s_cfgAcc.mask & 0x02u) || s_cfgAcc.curve_type != v) {
          changed = true;
        }
        s_cfgAcc.curve_type = v;
        s_cfgAcc.mask |= 0x02u;
        APP_SLOG("[WM] CFG 0245 curve=%d\n", (int)v);
      }
      break;
    case CFG_BACKUP:
      if (readCfgRegSoft((uint16_t)WINDMI_REG_BACKUP_HEATER, &raw)) {
        if (!(s_cfgAcc.mask & 0x04u) || s_cfgAcc.backup_heater != raw) {
          changed = true;
        }
        s_cfgAcc.backup_heater = raw;
        s_cfgAcc.mask |= 0x04u;
        APP_SLOG("[WM] CFG 0259 backup=%u\n", (unsigned)raw);
      }
      break;
    case CFG_MIN_OAT:
      if (readCfgRegSoft((uint16_t)WINDMI_REG_MIN_OAT_HEAT, &raw)) {
        const int16_t v = (int16_t)raw;
        if (!(s_cfgAcc.mask & 0x08u) || s_cfgAcc.min_oat_heat_x10 != v) {
          changed = true;
        }
        s_cfgAcc.min_oat_heat_x10 = v;
        s_cfgAcc.mask |= 0x08u;
        APP_SLOG("[WM] CFG 0202 minOAT=%d\n", (int)v);
      }
      break;
    case CFG_IBH_WARM:
      if (readCfgRegSoft((uint16_t)WINDMI_REG_IBH_WARMUP, &raw)) {
        if (!(s_cfgAcc.mask & 0x10u) || s_cfgAcc.ibh_warmup_min != raw) {
          changed = true;
        }
        s_cfgAcc.ibh_warmup_min = raw;
        s_cfgAcc.mask |= 0x10u;
        APP_SLOG("[WM] CFG 025A warmup=%u\n", (unsigned)raw);
      }
      break;
    case CFG_IBH_DT:
      if (readCfgRegSoft((uint16_t)WINDMI_REG_IBH_DELTA_T, &raw)) {
        const int16_t v = (int16_t)raw;
        if (!(s_cfgAcc.mask & 0x20u) || s_cfgAcc.ibh_delta_t_x10 != v) {
          changed = true;
        }
        s_cfgAcc.ibh_delta_t_x10 = v;
        s_cfgAcc.mask |= 0x20u;
        APP_SLOG("[WM] CFG 025B ibhDT=%d\n", (int)v);
      }
      break;
    case CFG_IBH_OAT:
      if (readCfgRegSoft((uint16_t)WINDMI_REG_IBH_OAT, &raw)) {
        const int16_t v = (int16_t)raw;
        if (!(s_cfgAcc.mask & 0x40u) || s_cfgAcc.ibh_oat_x10 != v) {
          changed = true;
        }
        s_cfgAcc.ibh_oat_x10 = v;
        s_cfgAcc.mask |= 0x40u;
        APP_SLOG("[WM] CFG 025C ibhOAT=%d\n", (int)v);
      }
      break;
    case CFG_PUMP_DT:
      if (readCfgRegSoft((uint16_t)WINDMI_REG_PUMP_DELTA_T, &raw)) {
        const int16_t v = (int16_t)raw;
        if (!(s_cfgAcc.mask & 0x80u) || s_cfgAcc.pump_delta_t_x10 != v) {
          changed = true;
        }
        s_cfgAcc.pump_delta_t_x10 = v;
        s_cfgAcc.mask |= 0x80u;
        APP_SLOG("[WM] CFG 0239 pumpDT=%d\n", (int)v);
      }
      break;
    case CFG_REQ_FREQ:
      if (readCfgRegSoft((uint16_t)WINDMI_REG_REQ_COMP_FREQ, &raw)) {
        lgModelSetMbLiveExtras((int16_t)raw, true);
        APP_SLOG("[WM] CFG 100F reqHz=%d\n", (int)(int16_t)raw);
      }
      break;
    default:
      break;
  }

  ++s_cfgStep;
  if (s_cfgStep >= CFG_COUNT) {
    const bool wasValid = s_cfgAcc.valid;
    s_cfgAcc.valid = (s_cfgAcc.mask != 0u);
    if (changed || s_cfgAcc.valid != wasValid) {
      lgModelSetMbHpConfig(&s_cfgAcc);
    }
    APP_SLOG("[WM] CFG done mask=0x%02X valid=%d\n",
                  (unsigned)s_cfgAcc.mask, (int)s_cfgAcc.valid);
    s_cfgNeedPoll = false;
    s_cfgStep = 0;
  } else if (changed) {
    // Držet valid=true při refresh — UI nebliká na "-"
    lgModelSetMbHpConfig(&s_cfgAcc);
  }
  return true;
}

void runLiveStep() {
  if (s_doSp) {
    s_doSp = false;
    runSp();
    return;
  }

  switch (s_step) {
    case STEP_TEMPS:
      runTemps();
      break;
    case STEP_MODE:
      runMode();
      break;
    case STEP_LOAD:
      runLoad();
      break;
    case STEP_COMP:
      runComp();
      break;
    case STEP_PUMP:
      runPump();
      break;
    case STEP_FLOW:
      runFlow();
      break;
    case STEP_QUIET:
      runQuiet();
      break;
    case STEP_ALARM:
      runAlarm();
      break;
    default:
      break;
  }

  s_step = (uint8_t)((s_step + 1u) % STEP_COUNT);
  if (s_step == 0) {
    ++s_cycle;
    if ((s_cycle % 4u) == 0u) {
      s_doSp = true;
    }
    // Na otevřené config obrazovce občas obnovit registry
    if (s_cfgScreen && !s_cfgNeedPoll && (s_cycle % 8u) == 0u) {
      s_cfgNeedPoll = true;
      s_cfgStep = 0;
    }
  }
}

}  // namespace

void rotensoBusInit(void) {
  pinMode(WINDMI_RS485_DIR_PIN, OUTPUT);
  dirTx(false);

  MbSerial.begin(WINDMI_MB_BAUD, SERIAL_8N1, WINDMI_RS485_RX_PIN,
                 WINDMI_RS485_TX_PIN);
  s_ready = true;
  s_lastTxMs = 0;
  s_step = STEP_TEMPS;
  s_cycle = 0;
  s_doSp = false;
  s_cfgQHead = s_cfgQTail = s_cfgQCount = 0;
  s_cfgScreen = false;
  s_cfgNeedPoll = false;
  s_cfgStep = 0;
  memset(&s_cfgAcc, 0, sizeof(s_cfgAcc));
  s_ok = 0;
  s_fail = 0;
  s_lastOkMs = 0;
  s_writeNextMs = 0;
  s_setting = s_running = 0;
  s_compX10 = 0;
  s_pump = s_quiet = s_flow = s_load = 0;
  s_loadOk = false;

  Serial.printf("[WM] init Serial1 RX=%d TX=%d DIR=%d baud=%u slave=%u "
                "READ=0x%02X WRITE=0x%02X (TEMP+MODE+LEDs+SP+CFG)\n",
                WINDMI_RS485_RX_PIN, WINDMI_RS485_TX_PIN, WINDMI_RS485_DIR_PIN,
                (unsigned)WINDMI_MB_BAUD, (unsigned)WINDMI_MB_SLAVE,
                (unsigned)WINDMI_FC_READ, (unsigned)WINDMI_FC_WRITE);
}

void rotensoBusTick(void) {
  if (!s_ready) {
    return;
  }

  if (processOneWrite()) {
    return;
  }

  if (runConfigStep()) {
    return;
  }

  runLiveStep();
}

bool rotensoBusIsReady(void) {
  return s_ready;
}

unsigned long rotensoBusOkCount(void) {
  return s_ok;
}

unsigned long rotensoBusFailCount(void) {
  return s_fail;
}

void rotensoBusSetConfigScreenActive(bool active) {
  s_cfgScreen = active;
  if (active) {
    s_cfgNeedPoll = true;
    s_cfgStep = 0;
  }
}

bool rotensoBusQueueConfigWrite(uint16_t addr, uint16_t value) {
  if (!s_ready) {
    return false;
  }
  return cfgQPush(addr, value);
}

void rotensoBusRequestConfigPoll(void) {
  s_cfgNeedPoll = true;
  s_cfgStep = 0;
}
