// climate_energy.cpp — ΔEnergy z PZEM + el. topení (3 kW) + historie
#include "climate_energy.h"

#include "net_ota.h"
#include "net_wifi_mgr.h"
#include "storage_config_nvs.h"
#include "bus_lg_model.h"
#include "bus_lg_protocol.h"

#include <Arduino.h>
#include <math.h>
#include <string.h>
#include <time.h>

namespace {

constexpr uint32_t kSaveIntervalMs = 5UL * 60UL * 1000UL;        // meta (den/měsíc/sezóna) ~5 min
constexpr uint32_t kPowerSaveIntervalMs = 5UL * 60UL * 1000UL;   // graf příkonu ~5 min
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
/** Layout pevný — neměnit velikost (NVS exact/compat load). */
constexpr uint16_t kMetaVersion = 2;

uint16_t* s_weekPower = nullptr;  // 7 * 1440 (W + ENERGY_PWR_AUX_BIT)
EnergyMeta s_meta{};
/** RAM-only: kolik z day_kwh[0] už je v season_month (ne do NVS — měnilo by size). */
float s_day0AppliedSeasonKwh = 0.0f;
/** false = v NVS je klíč, ale load selhal → NIKDY nepřepisovat flash prázdnem. */
bool s_allowMetaPersist = false;
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
bool s_prunedOldWeekDays = false;

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

bool ymdToTm(int ymd, struct tm* out) {
  if (!out || ymd < 20000101) {
    return false;
  }
  memset(out, 0, sizeof(*out));
  out->tm_year = ymd / 10000 - 1900;
  out->tm_mon = (ymd / 100) % 100 - 1;
  out->tm_mday = ymd % 100;
  out->tm_hour = 12;
  out->tm_isdst = -1;
  return mktime(out) != (time_t)-1;
}

int ymdAddDays(int ymd, int days) {
  struct tm tmLocal{};
  if (!ymdToTm(ymd, &tmLocal)) {
    return ymd;
  }
  time_t sec = mktime(&tmLocal);
  if (sec == (time_t)-1) {
    return ymd;
  }
  sec += static_cast<time_t>(days) * 86400;
  if (localtime_r(&sec, &tmLocal) == nullptr) {
    return ymd;
  }
  return ymdFromTm(tmLocal);
}

float sumSeasonMonthsKwh(void) {
  float sum = 0.0f;
  for (int i = 0; i < ENERGY_SEASON_MONTHS; ++i) {
    sum += s_meta.season_month_kwh[i];
  }
  return sum;
}

void markMetaDirty() {
  s_metaDirty = true;
}

void syncDayToToday(int todayYmd);

void ensureYearSlot(int seasonYear) {
  if (seasonYear <= 0) {
    return;
  }
  if (s_meta.year_id[0] == seasonYear) {
    return;
  }
  // Najdi existující sezónu a vytáhni dopředu
  for (int i = 0; i < ENERGY_YEAR_SLOTS; ++i) {
    if (s_meta.year_id[i] == seasonYear) {
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
  // Nová sezóna — posuň archiv (starý slot 0 → 1)
  memmove(&s_meta.year_id[1], &s_meta.year_id[0],
          (ENERGY_YEAR_SLOTS - 1) * sizeof(int32_t));
  memmove(&s_meta.year_kwh[1], &s_meta.year_kwh[0],
          (ENERGY_YEAR_SLOTS - 1) * sizeof(float));
  s_meta.year_id[0] = seasonYear;
  s_meta.year_kwh[0] = 0.0f;
  markMetaDirty();
}

void ensureSeason(int seasonYear) {
  if (seasonYear <= 0) {
    return;
  }
  if (s_meta.season_year == seasonYear) {
    ensureYearSlot(seasonYear);
    return;
  }
  const int32_t prev = s_meta.season_year;
  if (prev != 0) {
    // Uzavři starou sezónu v archivu (součet měsíců → slot 0), pak nový slot.
    if (s_meta.year_id[0] == prev) {
      s_meta.year_kwh[0] = sumSeasonMonthsKwh();
    }
    for (int i = 0; i < ENERGY_SEASON_MONTHS; ++i) {
      s_meta.season_month_kwh[i] = 0.0f;
    }
  }
  s_meta.season_year = seasonYear;
  ensureYearSlot(seasonYear);
  if (prev == 0 && s_meta.year_id[0] == seasonYear) {
    // První přiřazení po loadu — měsíce z NVS nech, slot sezóny sjednoť.
    s_meta.year_kwh[0] = sumSeasonMonthsKwh();
  }
  markMetaDirty();
  s_histGen++;
}

void syncCurrentSeasonArchiveSlot() {
  if (s_meta.season_year <= 0) {
    return;
  }
  ensureYearSlot(static_cast<int>(s_meta.season_year));
  const float sum = sumSeasonMonthsKwh();
  if (s_meta.year_id[0] == s_meta.season_year &&
      (s_meta.year_kwh[0] + kWhEps < sum || s_meta.year_kwh[0] > sum + kWhEps)) {
    s_meta.year_kwh[0] = sum;
    markMetaDirty();
  }
}

void creditSeasonForYmd(int ymd, float deltaKwh) {
  if (deltaKwh < kWhEps || ymd < 20000101) {
    return;
  }
  struct tm t{};
  if (!ymdToTm(ymd, &t)) {
    return;
  }
  const int month = t.tm_mon + 1;
  const int year = t.tm_year + 1900;
  const int seasonYear = climateEnergySeasonYear(year, month);
  const int seasonIdx = climateEnergySeasonMonthIndex(month);
  ensureSeason(seasonYear);
  if (seasonIdx >= 0) {
    s_meta.season_month_kwh[seasonIdx] += deltaKwh;
    if (s_meta.year_id[0] == seasonYear) {
      s_meta.year_kwh[0] += deltaKwh;
    }
    s_meta.last_month = month;
    s_meta.last_cal_year = year;
    markMetaDirty();
    s_histGen++;
  }
}

/** Dopočet dne do měsíce/sezóny (např. spotřeba jen v day_kwh bez applyDelta). */
void closeCurrentDayBucket() {
  const int ymd = static_cast<int>(s_meta.day_ymd[0]);
  if (ymd <= 0) {
    return;
  }
  const float gap = s_meta.day_kwh[0] - s_day0AppliedSeasonKwh;
  if (gap >= kWhEps) {
    creditSeasonForYmd(ymd, gap);
    s_day0AppliedSeasonKwh = s_meta.day_kwh[0];
    markMetaDirty();
  }
}

bool applyDeltaWithTm(float deltaKwh, const struct tm& t) {
  if (deltaKwh < kWhEps) {
    return false;
  }

  const int ymd = ymdFromTm(t);
  const int month = t.tm_mon + 1;
  const int year = t.tm_year + 1900;
  const int seasonYear = climateEnergySeasonYear(year, month);
  const int seasonIdx = climateEnergySeasonMonthIndex(month);

  syncDayToToday(ymd);

  ensureSeason(seasonYear);

  s_meta.day_kwh[0] += deltaKwh;
  if (seasonIdx >= 0) {
    s_meta.season_month_kwh[seasonIdx] += deltaKwh;
    s_day0AppliedSeasonKwh += deltaKwh;
  }
  // Archiv slot 0 = aktuální sezóna (průběžně)
  if (s_meta.year_id[0] == seasonYear) {
    s_meta.year_kwh[0] += deltaKwh;
  }
  s_meta.last_month = month;
  s_meta.last_cal_year = year;
  markMetaDirty();
  s_histGen++;
  return true;
}

bool applyDeltaFromYmd(float deltaKwh, int ymd) {
  struct tm t{};
  if (!ymdToTm(ymd, &t)) {
    return false;
  }
  return applyDeltaWithTm(deltaKwh, t);
}

void rollDayOne(int todayYmd) {
  // Měsíc/sezóna už rostou průběžně; close jen dorovná případný gap (bez NTP).
  closeCurrentDayBucket();
  const int prevYmd = static_cast<int>(s_meta.day_ymd[0]);
  memmove(&s_weekPower[ENERGY_MINUTES_PER_DAY], &s_weekPower[0],
          (ENERGY_WEEK_DAYS - 1) * ENERGY_MINUTES_PER_DAY * sizeof(uint16_t));
  memset(&s_weekPower[0], 0, ENERGY_MINUTES_PER_DAY * sizeof(uint16_t));

  for (int i = ENERGY_WEEK_DAYS - 1; i > 0; --i) {
    s_meta.day_ymd[i] = s_meta.day_ymd[i - 1];
    s_meta.day_kwh[i] = s_meta.day_kwh[i - 1];
  }
  if (prevYmd != 0) {
    s_meta.day_ymd[0] = ymdAddDays(prevYmd, 1);
  } else {
    s_meta.day_ymd[0] = todayYmd;
  }
  s_meta.day_kwh[0] = 0.0f;
  s_day0AppliedSeasonKwh = 0.0f;
  s_meta.last_ymd = s_meta.day_ymd[0];
  // Včerejšek hned do NVS (slot 1) — jinak při rebootu stačí zápis prázdného
  // en_pw0 a celý denní graf je pryč. Zbytek týdne doplní persist.
  s_powerDirty = true;
  s_weekFullDirty = true;
  s_weekSaveDay = 0;
  markMetaDirty();
  s_histGen++;
  if (!netOtaIsBusy() && s_weekPower) {
    storageSaveEnergyWeekPowerDay(1, &s_weekPower[ENERGY_MINUTES_PER_DAY]);
  }
}

void syncDayToToday(int todayYmd) {
  if (s_meta.day_ymd[0] == 0) {
    s_meta.day_ymd[0] = todayYmd;
    s_meta.day_kwh[0] = 0.0f;
    s_day0AppliedSeasonKwh = 0.0f;
    s_meta.last_ymd = todayYmd;
    markMetaDirty();
    return;
  }
  if (static_cast<int>(s_meta.day_ymd[0]) > todayYmd) {
    // NTP/čas skočil zpět — historii nezašlapávat.
    s_meta.last_ymd = todayYmd;
    return;
  }
  int guard = 0;
  while (static_cast<int>(s_meta.day_ymd[0]) < todayYmd &&
         guard < ENERGY_WEEK_DAYS) {
    rollDayOne(todayYmd);
    guard++;
  }
  if (static_cast<int>(s_meta.day_ymd[0]) < todayYmd) {
    // Výpadek > 7 dní — starší sloty už odrolovány, dnešek nastav natvrdo.
    s_meta.day_ymd[0] = todayYmd;
    s_meta.day_kwh[0] = 0.0f;
    s_day0AppliedSeasonKwh = 0.0f;
    s_meta.last_ymd = todayYmd;
    s_powerDirty = true;
    s_weekFullDirty = true;
    s_weekSaveDay = 0;
    markMetaDirty();
    s_histGen++;
  }
}

void persistIfNeeded(bool force) {
  // NVS + OTA sdílí flash — zápis během uploadu zasekne espota.
  // Meta (den/měsíc/sezóna) i příkon jen v intervalu — ne při každém ΔE.
  if (netOtaIsBusy()) {
    return;
  }
  const uint32_t now = millis();
  const bool metaDue = force || ((now - s_lastSaveMs) >= kSaveIntervalMs);
  bool powerDue = force;
  if (!powerDue && s_weekFullDirty) {
    // Po půlnoci: jeden den na tick (ne 30 min × 7). Meta zůstává na intervalu.
    powerDue = true;
  } else if (!powerDue && s_powerDirty) {
    if (s_lastPowerSaveMs == 0) {
      powerDue = (now >= kPowerSaveFirstMs);
    } else {
      powerDue = (now - s_lastPowerSaveMs) >= kPowerSaveIntervalMs;
    }
  }

  if (s_metaDirty && metaDue) {
    if (!s_allowMetaPersist) {
      Serial.println("[ENERGY] skip meta save — NVS load failed, protect flash");
    } else {
      storageSaveEnergyMeta(&s_meta, sizeof(s_meta));
      s_metaDirty = false;
      s_lastSaveMs = now;
      // S meta i dnešní křivku — ať graf přežije reboot stejně jako kWh.
      if (s_powerDirty && s_weekPower && !s_weekFullDirty) {
        storageSaveEnergyWeekPowerDay(0, &s_weekPower[0]);
        s_powerDirty = false;
        s_lastPowerSaveMs = now;
      }
    }
  }

  if ((s_powerDirty || s_weekFullDirty) && s_weekPower && powerDue) {
    if (s_weekFullDirty || force) {
      // Jen včera (1) a dnes (0) — starší dny v NVS přetekly flash.
      static const int kOrder[] = {1, 0};
      constexpr int kOrderN = 2;
      int idx = s_weekSaveDay;
      if (idx < 0 || idx >= kOrderN) {
        idx = 0;
      }
      const int d = kOrder[idx];
      storageSaveEnergyWeekPowerDay(d, &s_weekPower[d * ENERGY_MINUTES_PER_DAY]);
      s_weekSaveDay = idx + 1;
      s_lastPowerSaveMs = now;
      if (s_weekSaveDay >= kOrderN) {
        s_weekSaveDay = 0;
        s_weekFullDirty = false;
        s_powerDirty = false;
      }
    } else {
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
  if (periods > 0) {
    const float dKwh = kAuxDeltaKwh * (float)periods;
    if (tOpt) {
      applyDeltaWithTm(dKwh, *tOpt);
    } else {
      const int ymd = static_cast<int>(s_meta.day_ymd[0]);
      if (ymd > 0) {
        applyDeltaFromYmd(dKwh, ymd);
      } else if (s_meta.last_ymd > 0) {
        applyDeltaFromYmd(dKwh, static_cast<int>(s_meta.last_ymd));
      } else {
        s_meta.day_kwh[0] += dKwh;
        markMetaDirty();
        s_histGen++;
      }
    }
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
  s_allowMetaPersist = true;
  s_day0AppliedSeasonKwh = 0.0f;
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
  s_day0AppliedSeasonKwh = 0.0f;
  s_allowMetaPersist = false;

  EnergyMeta loaded{};
  const bool metaOk = storageLoadEnergyMeta(&loaded, sizeof(loaded)) &&
                      loaded.magic == kMetaMagic;
  if (metaOk) {
    s_meta = loaded;
    if (s_meta.version != kMetaVersion) {
      s_meta.version = kMetaVersion;
      markMetaDirty();
    }
    // Po bootu: den už je v měsíci/sezóně (průběžný apply); gap tracker = dnes.
    s_day0AppliedSeasonKwh = s_meta.day_kwh[0];
    s_allowMetaPersist = true;
    syncCurrentSeasonArchiveSlot();
  } else if (!storageEnergyMetaKeyExists()) {
    // První start — smíme založit meta.
    s_allowMetaPersist = true;
    markMetaDirty();
    Serial.println("[ENERGY] no en_meta key — starting fresh counters");
  } else {
    // Klíč je, ale nečitelný — nepřepisuj flash.
    Serial.println(
        "[ENERGY] en_meta unreadable — RAM empty, leave flash untouched");
    s_allowMetaPersist = false;
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
  uint32_t day0Nz = 0;
  if (s_weekPower) {
    for (int i = 0; i < ENERGY_MINUTES_PER_DAY; ++i) {
      if (s_weekPower[i] != 0) {
        ++day0Nz;
      }
    }
  }
  s_histGen++;
  Serial.printf(
      "[ENERGY] init meta=%d e_prev=%lu Wh season=%ld week_nz=%lu day0_nz=%lu\n",
      metaOk ? 1 : 0, (unsigned long)s_meta.e_prev_wh,
      (long)s_meta.season_year, (unsigned long)nz, (unsigned long)day0Nz);
}

void climateEnergyTick(void) {
  if (netOtaIsBusy()) {
    return;
  }
  // Jednou po Wi‑Fi: smazat staré en_p2..en_p6 (ne při load/boot — shazuje SDIO).
  if (!s_prunedOldWeekDays && netWifiIsConnected() && !netWifiIsBusy()) {
    storagePruneEnergyWeekPowerOldDays();
    s_prunedOldWeekDays = true;
  }
  struct tm t{};
  const bool haveTime = localNow(&t);
  if (haveTime) {
    const int ymd = ymdFromTm(t);
    if (s_meta.last_ymd != 0 && ymd != s_meta.last_ymd) {
      syncDayToToday(ymd);
    } else if (s_meta.day_ymd[0] == 0) {
      s_meta.day_ymd[0] = ymd;
      s_meta.last_ymd = ymd;
      markMetaDirty();
    }
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
    // Nejdřív srovnej den — jinak body padají do včerejšího bufferu.
    syncDayToToday(ymdFromTm(t));
    const uint16_t prev = s_weekPower[minuteOfDay];
    const uint16_t prevW = (uint16_t)(prev & ENERGY_PWR_W_MASK);
    // PWR je ~1×/min průměr; při více vzorcích drž peak minuty (ne přepsat 0 W).
    uint16_t cellW = s_powerW;
    if (prevW > cellW) {
      cellW = prevW;
    }
    uint16_t cell = cellW;
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
  bool prevAdvanced = false;
  if (!s_meta.have_prev) {
    s_meta.e_prev_wh = energyWh;
    s_meta.have_prev = 1;
    markMetaDirty();
    prevAdvanced = true;
  } else if (energyReset || energyWh < s_meta.e_prev_wh) {
    // Po resetu: nová hodnota = spotřeba od nuly
    deltaKwh = (float)energyWh / 1000.0f;
    s_meta.e_prev_wh = energyWh;
    markMetaDirty();
    prevAdvanced = true;
  } else if (energyWh != s_meta.e_prev_wh) {
    deltaKwh = (float)(energyWh - s_meta.e_prev_wh) / 1000.0f;
  }

  if (deltaKwh > kWhEps) {
    if (haveTime) {
      applyDeltaWithTm(deltaKwh, t);
    } else {
      const int ymd = static_cast<int>(s_meta.day_ymd[0]);
      if (ymd > 0) {
        applyDeltaFromYmd(deltaKwh, ymd);
      } else if (s_meta.last_ymd > 0) {
        applyDeltaFromYmd(deltaKwh, static_cast<int>(s_meta.last_ymd));
      } else {
        s_meta.day_kwh[0] += deltaKwh;
        markMetaDirty();
        s_histGen++;
      }
    }
    if (!prevAdvanced) {
      s_meta.e_prev_wh = energyWh;
      markMetaDirty();
    }
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

float climateEnergySeasonTotalKwh(void) { return sumSeasonMonthsKwh(); }

float climateEnergyYearKwh(void) { return climateEnergySeasonTotalKwh(); }

const char* climateEnergySeasonMonthLabel(void) {
  static const char* kNames[ENERGY_SEASON_MONTHS] = {
      "Zář", "Říj", "Lis", "Pro", "Led", "Úno", "Bře", "Dub", "Kvě"};
  struct tm t{};
  if (!localNow(&t)) {
    return "-";
  }
  const int idx = climateEnergySeasonMonthIndex(t.tm_mon + 1);
  if (idx < 0) {
    return "mimo sezónu";
  }
  return kNames[idx];
}

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
