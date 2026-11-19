// climate_comp_stats.cpp — start kompresoru = potvrzený OFF→ON (ne flicker Hz)
#include "climate_comp_stats.h"

#include "bus_lg_model.h"
#include "bus_rotenso_config.h"

#include <Arduino.h>
#include <time.h>

namespace {

constexpr uint32_t kHourMs = 60u * 60u * 1000u;
/**
 * Potvrzení vypnutí: Hz==0 musí vydržet tak dlouho, aby to nebyl glitch
 * během běhu / odmrazování. Graf spotřeby je 5 min — krátké díry tam
 * stejně neuvidíš jako samostatný start.
 */
constexpr uint32_t kOffConfirmMs = 120000u;  // 2 min
/** Potvrzení běhu: krátký spike Hz nepočítat. */
constexpr uint32_t kOnConfirmMs = 30000u;  // 30 s
/** Minimální frekvence „běží“ (×10) — pod tím šum / idle. */
constexpr int16_t kMinHzX10 = 50;  // 5,0 Hz
constexpr int kHourRing = 64;

enum class CompPhase : uint8_t {
  Unknown = 0,
  Off,
  On,
};

CompPhase s_phase = CompPhase::Unknown;
uint32_t s_rawChangeMs = 0;
bool s_rawOn = false;
bool s_haveRaw = false;

uint16_t s_dayCount = 0;
int32_t s_dayYmd = 0;

uint32_t s_hourMs[kHourRing] = {};
uint8_t s_hourHead = 0;
uint8_t s_hourCount = 0;

int ymdNow(void) {
  time_t now = time(nullptr);
  if (now < 1700000000) {
    return 0;
  }
  struct tm tmLocal {};
  if (localtime_r(&now, &tmLocal) == nullptr) {
    return 0;
  }
  return (tmLocal.tm_year + 1900) * 10000 + (tmLocal.tm_mon + 1) * 100 +
         tmLocal.tm_mday;
}

void pruneHour(uint32_t nowMs) {
  while (s_hourCount > 0) {
    const uint32_t t =
        s_hourMs[(s_hourHead + kHourRing - s_hourCount) % kHourRing];
    if ((nowMs - t) < kHourMs) {
      break;
    }
    --s_hourCount;
  }
}

void recordStart(uint32_t nowMs) {
  const int ymd = ymdNow();
  if (ymd > 0) {
    if (s_dayYmd != 0 && s_dayYmd != ymd) {
      s_dayCount = 0;
    }
    s_dayYmd = ymd;
  }
  if (s_dayCount < 0xFFFEu) {
    ++s_dayCount;
  }

  pruneHour(nowMs);
  s_hourMs[s_hourHead] = nowMs;
  s_hourHead = (uint8_t)((s_hourHead + 1) % kHourRing);
  if (s_hourCount < kHourRing) {
    ++s_hourCount;
  }
}

/**
 * Raw běh z live snapu.
 * Při neplatném/výpadku snapu drž poslední raw — jinak MB glitch = falešné OFF.
 */
bool readRawOn(bool* haveSample) {
  WindmiLiveSnap live {};
  lgModelReadLiveSnap(&live);
  if (!live.valid) {
    if (haveSample) {
      *haveSample = false;
    }
    return s_rawOn;
  }
  if (haveSample) {
    *haveSample = true;
  }
  // Odmrazování: frekvence často kolísá / padá na 0 — nebrat jako cyklus topení.
  if (live.running_mode == (uint16_t)WINDMI_RUN_DEFROST) {
    return s_rawOn;
  }
  return live.comp_freq_x10 >= kMinHzX10;
}

}  // namespace

void climateCompStatsTick(void) {
  const uint32_t nowMs = millis();
  bool haveSample = false;
  const bool raw = readRawOn(&haveSample);

  if (haveSample) {
    if (!s_haveRaw) {
      s_haveRaw = true;
      s_rawOn = raw;
      s_rawChangeMs = nowMs;
      // Boot: přijmi stav bez započítání startu.
      s_phase = raw ? CompPhase::On : CompPhase::Off;
    } else if (raw != s_rawOn) {
      s_rawOn = raw;
      s_rawChangeMs = nowMs;
    }
  }

  if (s_phase == CompPhase::Unknown) {
    pruneHour(nowMs);
    return;
  }

  const uint32_t held = nowMs - s_rawChangeMs;

  if (s_phase == CompPhase::On) {
    // Vypnutí až po dlouhém potvrzeném Hz≈0 (ne krátký glitch).
    if (!s_rawOn && held >= kOffConfirmMs) {
      s_phase = CompPhase::Off;
    }
  } else {  // Off
    // Start až po potvrzeném běhu — filtruje spike.
    if (s_rawOn && held >= kOnConfirmMs) {
      recordStart(nowMs);
      s_phase = CompPhase::On;
    }
  }

  pruneHour(nowMs);

  const int ymd = ymdNow();
  if (ymd > 0 && s_dayYmd != 0 && s_dayYmd != ymd) {
    s_dayYmd = ymd;
    s_dayCount = 0;
  }
}

uint16_t climateCompStartsToday(void) { return s_dayCount; }

uint16_t climateCompStartsLastHour(void) {
  pruneHour(millis());
  return s_hourCount;
}
