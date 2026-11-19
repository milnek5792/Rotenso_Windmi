// climate_comp_stats.h — diagnostika: počet startů kompresoru
#ifndef CLIMATE_COMP_STATS_H
#define CLIMATE_COMP_STATS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void climateCompStatsTick(void);

/** Starty dnes (kalendářní den při platném čase; jinak od bootu). */
uint16_t climateCompStartsToday(void);

/** Starty za posledních 60 minut (podle millis). */
uint16_t climateCompStartsLastHour(void);

#ifdef __cplusplus
}
#endif

#endif
