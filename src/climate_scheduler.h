#ifndef CLIMATE_SCHEDULER_H
#define CLIMATE_SCHEDULER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void climateSchedulerInit();
void climateSchedulerTick();

/** Zap/vyp tichý režim TČ (Night window 0206H/0207H). */
void climateQuietSet(bool on);
/** Aktuální požadavek tichého režimu (UI přepínač). */
bool climateQuietIsOn(void);
/** Přepne tichý režim TČ (ne plánový útlum). */
void climateTichyManualToggle();

/**
 * Tichý režim byl auto-vypnut kvůli nízké OAT a ještě není povolen
 * znovu (hystereze). Signálka = varovná barva.
 */
bool climateQuietColdBlocked(void);

/** Práh OAT pro auto-vypnutí (°C). */
float climateQuietOatOffC(void);
/** Úprava prahu o ±1 °C (uloží NVS). */
void climateQuietOatOffAdjust(int deltaC);

#ifdef __cplusplus
}
#endif

#endif
