// climate_energy.cpp — ΔEnergy z PZEM + el. topení (3 kW) + historie
#include "climate_energy.h"

#include "net_ota.h"
#include "storage_config_nvs.h"
#include "bus_lg_model.h"
#include "bus_lg_protocol.h"

#include <Arduino.h>
#include <math.h>
#include <string.h>
#include <time.h>

namespace {

constexpr uint32_t kSaveIntervalMs = 30UL * 60UL * 1000UL;       // meta ~30 min
constexpr uint32_t kPowerSaveIntervalMs = 30UL * 60UL * 1000UL;  // příkon ~30 min
constexpr uint32_t kPowerSaveFirstMs = 45UL * 1000UL;            // první zápis po dirty (boot)
constexpr float kWhEps = 0.0005f;

/** Přídavné topení — jiná fáze, nejde měřit PZEM; integrace při A0 B2.0x04. */
constexpr uint16_t kAuxHeatW = 3000;
constexpr uint32_t kAuxPeriodMs = 10000;
// 3 kW × 10 s = 30000/3600 Wh = 8.333 Wh
constexpr float kAuxDeltaKwh = (float)kAuxHeatW * ((float)kAuxPeriodMs / 1000.0f) /
                               3600.0f / 1000.0f;

struct EnergyMeta {
  uint32_t magic;
  uint16_t version;
  uint16_t _pad;
  uint32_t e_prev_wh;
  uint8_t have_prev;
  uint8_t _pad2[3];
  int32_t day_ymd[ENERGY_WEEK_DAYS];
  float day_kwh[ENERGY_WEEK_DAYS];
  int32_t season_year;
  float season_month_kwh[ENERGY_SEASON_MONTHS];
  int32_t year_id[ENERGY_YEAR_SLOTS];
  float year_kwh[ENERGY_YEAR_SLOTS];
  int32_t last_ymd;
  int32_t last_month;
  int32_t last_cal_year;
};

constexpr uint32_t kMetaMagic = 0x454E5231u;  // ENR1
constexpr uint16_t kMetaVersion = 2;

uint16_t* s_weekPower = nullptr;  // 7 * 1440 (W + ENERGY_PWR_AUX_BIT)
EnergyMeta s_meta{};
bool s_ok = false;
uint16_t s_powerW = 0;  // poslední PZEM W (bez el. topení)
bool s_auxOn = false;
uint32_t s_lastAuxMs = 0;
int s_auxLatchMinute = -1;
uint32_t s_histGen = 1;
uint32_t s_lastSaveMs = 0;
uint32_t s_lastPowerSaveMs = 0;
bool s_metaDirty = false;
bool s_powerDirty = false;
bool s_weekFullDirty = false;
int s_weekSaveDay = 0;

int ymdFromTm(const struct tm& t) {
  return (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday;
}

bool localNow(struct tm* out) {
  if (!out) {
    return false;
  }
  time_t now = time(nullptr);
  if (now < 1700000000) {  // ~2023 — NTP ještě ne
    return false;
  }
  return localtime_r(&now, out) != nullptr;
}

void markMetaDirty() {
  s_metaDirty = true;
}

void ensureYearSlot(int year) {
  if (s_meta.year_id[0] == year) {
    return;
  }
  // Najdi existující
  for (int i = 0; i < ENERGY_YEAR_SLOTS; ++i) {
    if (s_meta.year_id[i] == year) {
      const int id = s_meta.year_id[i];
      const float kwh = s_meta.year_kwh[i];
      memmove(&s_meta.year_id[1], &s_meta.year_id[0],
              (size_t)i * sizeof(int32_t));
      memmove(&s_meta.year_kwh[1], &s_meta.year_kwh[0],
              (size_t)i * sizeof(float));
      s_meta.year_id[0] = id;
      s_meta.year_kwh[0] = kwh;
      markMetaDirty();
      return;
    }
  }
  // Nový rok — posuň historii
  memmove(&s_meta.year_id[1], &s_meta.year_id[0],
          (ENERGY_YEAR_SLOTS - 1) * sizeof(int32_t));
  memmove(&s_meta.year_kwh[1], &s_meta.year_kwh[0],
          (ENERGY_YEAR_SLOTS - 1) * sizeof(float));
  s_meta.year_id[0] = year;
  s_meta.year_kwh[0] = 0.0f;
  markMetaDirty();
}

void ensureSeason(int seasonYear) {
  if (s_meta.season_year == seasonYear) {
    return;
  }
  s_meta.season_year = seasonYear;
  for (int i = 0; i < ENERGY_SEASON_MONTHS; ++i) {
    s_meta.season_month_kwh[i] = 0.0f;
  }
  markMetaDirty();
}

void rollDay(int newYmd) {
  // Posuň týdenní buffer o 1 den (index 0 = dnes)
  memmove(&s_weekPower[ENERGY_MINUTES_PER_DAY], &s_weekPower[0],
          (ENERGY_WEEK_DAYS - 1) * ENERGY_MINUTES_PER_DAY * sizeof(uint16_t));
  memset(&s_weekPower[0], 0, ENERGY_MINUTES_PER_DAY * sizeof(uint16_t));

  for (int i = ENERGY_WEEK_DAYS - 1; i > 0; --i) {
    s_meta.day_ymd[i] = s_meta.day_ymd[i - 1];
    s_meta.day_kwh[i] = s_meta.day_kwh[i - 1];
  }
  s_meta.day_ymd[0] = newYmd;
  s_meta.day_kwh[0] = 0.0f;
  s_meta.last_ymd = newYmd;
  s_powerDirty = true;
  s_weekFullDirty = true;  // posunuté dny zapsat najednou (mimo minutový PWR)
  markMetaDirty();
  s_histGen++;
}

void applyDelta(float deltaKwh, const struct tm& t) {
  if (deltaKwh < 0.0f) {
    deltaKwh = 0.0f;
  }
  if (deltaKwh < kWhEps) {
    return;
  }

  const int ymd = ymdFromTm(t);
  const int month = t.tm_mon + 1;
  const int year = t.tm_year + 1900;
  const int seasonYear = climateEnergySeasonYear(year, month);
  const int seasonIdx = climateEnergySeasonMonthIndex(month);

  if (s_meta.last_ymd != 0 && ymd != s_meta.last_ymd) {
    // Midnight / NTP catch-up — roll missing days at most 6
    int guard = 0;
    while (s_meta.last_ymd != ymd && guard < ENERGY_WEEK_DAYS) {
      rollDay(ymd);  // simplified: jump to today
      break;
    }
  }
  if (s_meta.day_ymd[0] != ymd) {
    if (s_meta.day_ymd[0] == 0) {
      s_meta.day_ymd[0] = ymd;
      s_meta.last_ymd = ymd;
    } else {
      rollDay(ymd);
    }
  }

  ensureYearSlot(year);
  ensureSeason(seasonYear);

  s_meta.day_kwh[0] += deltaKwh;
  s_meta.year_kwh[0] += deltaKwh;
  if (seasonIdx >= 0) {
    s_meta.season_month_kwh[seasonIdx] += deltaKwh;
  }
  s_meta.last_month = month;
  s_meta.last_cal_year = year;
  markMetaDirty();
  s_histGen++;
}

void persistIfNeeded(bool force) {
  // NVS + OTA sdílí flash — zápis během uploadu zasekne espota.
  if (netOtaIsBusy()) {
    return;
  }
  const uint32_t now = millis();
  const bool metaDue = force || ((now - s_lastSaveMs) >= kSaveIntervalMs);
  bool powerDue = force || s_weekFullDirty;
  if (!powerDue && s_powerDirty) {
    if (s_lastPowerSaveMs == 0) {
      // Po bootu / první dirty — zapsat brzy (ne čekat celý interval).
      powerDue = (now >= kPowerSaveFirstMs);
    } else {
      powerDue = (now - s_lastPowerSaveMs) >= kPowerSaveIntervalMs;
    }
  }

  if (s_metaDirty && metaDue) {
    storageSaveEnergyMeta(&s_meta, sizeof(s_meta));
    s_metaDirty = false;
    s_lastSaveMs = now;
  }

  if (s_powerDirty && s_weekPower && powerDue) {
    if (s_weekFullDirty || force) {
      // Po jednom dni na tick — 7× putBytes (~20 kB) najednou shazovalo UI/Wi‑Fi.
      const int d = (s_weekSaveDay >= 0 && s_weekSaveDay < ENERGY_WEEK_DAYS)
                        ? s_weekSaveDay
                        : 0;
      storageSaveEnergyWeekPowerDay(d, &s_weekPower[d * ENERGY_MINUTES_PER_DAY]);
      s_weekSaveDay = d + 1;
      if (s_weekSaveDay >= ENERGY_WEEK_DAYS) {
        s_weekSaveDay = 0;
        s_weekFullDirty = false;
        s_powerDirty = false;
        s_lastPowerSaveMs = now;
      }
    } else {
      // Běžný minutový vzorek: jen dnešek.
      storageSaveEnergyWeekPowerDay(0, &s_weekPower[0]);
      s_powerDirty = false;
      s_lastPowerSaveMs = now;
    }
  }
}

void latchAuxBit(int minuteOfDay) {
  if (!s_weekPower || minuteOfDay < 0 ||
      minuteOfDay >= ENERGY_MINUTES_PER_DAY) {
    return;
  }
  if ((s_weekPower[minuteOfDay] & ENERGY_PWR_AUX_BIT) != 0) {
    return;
  }
  s_weekPower[minuteOfDay] =
      (uint16_t)((s_weekPower[minuteOfDay] & ENERGY_PWR_W_MASK) |
                 ENERGY_PWR_AUX_BIT);
  s_powerDirty = true;
  s_histGen++;
}

void tickAuxHeat(const struct tm* tOpt) {
  const bool on =
      lgMaCerstoA0(5000) && lgJeElTopeni(lgModelA0Bajt(2, 0));
  s_auxOn = on;

  const uint32_t now = millis();
  if (!on) {
    s_lastAuxMs = 0;
    s_auxLatchMinute = -1;
    return;
  }

  if (s_lastAuxMs == 0) {
    s_lastAuxMs = now;
  }

  if (tOpt) {
    const int minuteOfDay = tOpt->tm_hour * 60 + tOpt->tm_min;
    if (minuteOfDay != s_auxLatchMinute) {
      s_auxLatchMinute = minuteOfDay;
      latchAuxBit(minuteOfDay);
    }
  }

  if ((now - s_lastAuxMs) < kAuxPeriodMs) {
    return;
  }
  // Integrovat celé periody (i po delším ticku)
  const uint32_t periods = (now - s_lastAuxMs) / kAuxPeriodMs;
  s_lastAuxMs += periods * kAuxPeriodMs;
  if (tOpt && periods > 0) {
    applyDelta(kAuxDeltaKwh * (float)periods, *tOpt);
  }
}

}  // namespace

int climateEnergySeasonMonthIndex(int calendarMonth1to12) {
  // Zář=9 → 0 … Pro=12 → 3, Led=1 → 4 … Kvě=5 → 8
  if (calendarMonth1to12 >= 9 && calendarMonth1to12 <= 12) {
    return calendarMonth1to12 - 9;
  }
  if (calendarMonth1to12 >= 1 && calendarMonth1to12 <= 5) {
    return calendarMonth1to12 + 3;
  }
  return -1;
}

int climateEnergySeasonYear(int calendarYear, int calendarMonth1to12) {
  if (calendarMonth1to12 >= 1 && calendarMonth1to12 <= 5) {
    return calendarYear - 1;
  }
  return calendarYear;
}

void climateEnergyClearHistory(void) {
  memset(&s_meta, 0, sizeof(s_meta));
  s_meta.magic = kMetaMagic;
  s_meta.version = kMetaVersion;
  s_meta.have_prev = 0;
  s_meta.e_prev_wh = 0;
  s_ok = false;
  s_powerW = 0;
  s_auxOn = false;
  s_lastAuxMs = 0;
  s_auxLatchMinute = -1;
  if (s_weekPower) {
    memset(s_weekPower, 0,
           ENERGY_WEEK_DAYS * ENERGY_MINUTES_PER_DAY * sizeof(uint16_t));
  }
  // Jen remove + malé meta — ne přepisovat 7× denní buffer (zasekává flash/OTA).
  storageClearEnergyHistory();
  storageSaveEnergyMeta(&s_meta, sizeof(s_meta));
  s_metaDirty = false;
  s_powerDirty = false;
  s_weekFullDirty = false;
  s_weekSaveDay = 0;
  s_histGen++;
  Serial.println("[ENERGY] history cleared (RAM+NVS)");
}

void climateEnergyInit(void) {
  if (!s_weekPower) {
    s_weekPower = (uint16_t*)ps_calloc(ENERGY_WEEK_DAYS * ENERGY_MINUTES_PER_DAY,
                                       sizeof(uint16_t));
    if (!s_weekPower) {
      s_weekPower = (uint16_t*)calloc(ENERGY_WEEK_DAYS * ENERGY_MINUTES_PER_DAY,
                                      sizeof(uint16_t));
    }
  }
  memset(&s_meta, 0, sizeof(s_meta));
  s_meta.magic = kMetaMagic;
  s_meta.version = kMetaVersion;

  EnergyMeta loaded{};
  const bool metaOk = storageLoadEnergyMeta(&loaded, sizeof(loaded)) &&
                      loaded.magic == kMetaMagic;
  if (metaOk) {
    s_meta = loaded;
    if (s_meta.version != kMetaVersion) {
      s_meta.version = kMetaVersion;
      markMetaDirty();
    }
  } else {
    Serial.println("[ENERGY] no saved meta — starting empty counters");
    storageSaveEnergyMeta(&s_meta, sizeof(s_meta));
  }

  uint32_t nz = 0;
  bool anyPwr = false;
  if (s_weekPower) {
    anyPwr = storageLoadEnergyWeekPower(
        s_weekPower, ENERGY_WEEK_DAYS * ENERGY_MINUTES_PER_DAY);
    if (anyPwr) {
      const size_t n = (size_t)ENERGY_WEEK_DAYS * ENERGY_MINUTES_PER_DAY;
      for (size_t i = 0; i < n; ++i) {
        if (s_weekPower[i] != 0) {
          ++nz;
        }
      }
    }
  }
  s_histGen++;
  Serial.printf(
      "[ENERGY] init meta=%d e_prev=%lu Wh season=%ld week_nz=%lu\n",
      metaOk ? 1 : 0, (unsigned long)s_meta.e_prev_wh,
      (long)s_meta.season_year, (unsigned long)nz);
}

void climateEnergyTick(void) {
  if (netOtaIsBusy()) {
    return;
  }
  struct tm t{};
  const bool haveTime = localNow(&t);
  if (haveTime) {
    const int ymd = ymdFromTm(t);
    if (s_meta.last_ymd != 0 && ymd != s_meta.last_ymd) {
      rollDay(ymd);
    } else if (s_meta.day_ymd[0] == 0) {
      s_meta.day_ymd[0] = ymd;
      s_meta.last_ymd = ymd;
      markMetaDirty();
    }
    ensureYearSlot(t.tm_year + 1900);
    ensureSeason(climateEnergySeasonYear(t.tm_year + 1900, t.tm_mon + 1));
  }
  tickAuxHeat(haveTime ? &t : nullptr);
  persistIfNeeded(false);
}

void climateEnergyOnSample(uint16_t avgPowerW, float energyKwh, bool energyReset) {
  s_powerW = (uint16_t)(avgPowerW & ENERGY_PWR_W_MASK);
  s_ok = true;

  struct tm t{};
  const bool haveTime = localNow(&t);
  const int minuteOfDay =
      haveTime ? (t.tm_hour * 60 + t.tm_min) : -1;

  if (s_weekPower && minuteOfDay >= 0 && minuteOfDay < ENERGY_MINUTES_PER_DAY) {
    if (s_meta.day_ymd[0] == 0 && haveTime) {
      s_meta.day_ymd[0] = ymdFromTm(t);
      s_meta.last_ymd = s_meta.day_ymd[0];
    }
    const uint16_t prev = s_weekPower[minuteOfDay];
    uint16_t cell = s_powerW;
    if (s_auxOn || (prev & ENERGY_PWR_AUX_BIT) != 0) {
      cell = (uint16_t)(cell | ENERGY_PWR_AUX_BIT);
    }
    s_weekPower[minuteOfDay] = cell;
    s_powerDirty = true;
    s_histGen++;
  }

  uint32_t energyWh = 0;
  if (energyKwh > 0.0f) {
    energyWh = (uint32_t)(energyKwh * 1000.0f + 0.5f);
  }

  float deltaKwh = 0.0f;
  if (!s_meta.have_prev) {
    s_meta.e_prev_wh = energyWh;
    s_meta.have_prev = 1;
    markMetaDirty();
  } else if (energyReset || energyWh < s_meta.e_prev_wh) {
    // Po resetu: nová hodnota = spotřeba od nuly
    deltaKwh = (float)energyWh / 1000.0f;
    s_meta.e_prev_wh = energyWh;
    markMetaDirty();
  } else if (energyWh != s_meta.e_prev_wh) {
    deltaKwh = (float)(energyWh - s_meta.e_prev_wh) / 1000.0f;
    s_meta.e_prev_wh = energyWh;
    markMetaDirty();
  }

  if (haveTime && deltaKwh > kWhEps) {
    applyDelta(deltaKwh, t);
  }

  // Flash zápis ne tady (UART RX / UI) — climateEnergyTick()
}

bool climateEnergyIsOk(void) { return s_ok; }

uint16_t climateEnergyPowerW(void) {
  uint32_t w = s_powerW;
  if (s_auxOn) {
    w += kAuxHeatW;
  }
  if (w > ENERGY_PWR_W_MASK) {
    w = ENERGY_PWR_W_MASK;
  }
  return (uint16_t)w;
}

bool climateEnergyAuxHeatOn(void) { return s_auxOn; }

float climateEnergyTodayKwh(void) { return s_meta.day_kwh[0]; }

float climateEnergyMonthKwh(void) {
  struct tm t{};
  if (!localNow(&t)) {
    return 0.0f;
  }
  const int idx = climateEnergySeasonMonthIndex(t.tm_mon + 1);
  if (idx < 0) {
    return 0.0f;
  }
  return s_meta.season_month_kwh[idx];
}

float climateEnergyYearKwh(void) { return s_meta.year_kwh[0]; }

bool climateEnergyDayPowerGet(int dayOffset, const uint16_t** outSamples,
                              float* outDayKwh, int* outYmd) {
  if (dayOffset < 0 || dayOffset >= ENERGY_WEEK_DAYS || !s_weekPower) {
    return false;
  }
  if (outSamples) {
    *outSamples = &s_weekPower[dayOffset * ENERGY_MINUTES_PER_DAY];
  }
  if (outDayKwh) {
    *outDayKwh = s_meta.day_kwh[dayOffset];
  }
  if (outYmd) {
    *outYmd = (int)s_meta.day_ymd[dayOffset];
  }
  return true;
}

uint32_t climateEnergyHistoryGen(void) { return s_histGen; }

float climateEnergySeasonMonthKwh(int seasonMonthIndex) {
  if (seasonMonthIndex < 0 || seasonMonthIndex >= ENERGY_SEASON_MONTHS) {
    return 0.0f;
  }
  return s_meta.season_month_kwh[seasonMonthIndex];
}

int climateEnergyCurrentSeasonYear(void) { return (int)s_meta.season_year; }

bool climateEnergyYearGet(int index, int* outYear, float* outKwh) {
  if (index < 0 || index >= ENERGY_YEAR_SLOTS) {
    return false;
  }
  if (s_meta.year_id[index] == 0) {
    return false;
  }
  if (outYear) {
    *outYear = (int)s_meta.year_id[index];
  }
  if (outKwh) {
    *outKwh = s_meta.year_kwh[index];
  }
  return true;
}
