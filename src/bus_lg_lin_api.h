// bus_lg_lin_api.h — legacy API (LIN odstraněn; stuby pro UI / session)
#ifndef LG_LIN_API_H
#define LG_LIN_API_H

#include <Arduino.h>

void lgBusInit();
void lgBusTick();

unsigned long lgPocetPaketu();
unsigned long lgPocetRxBajtu();
bool lgBusIsReady();
bool lgZapisBezi();

unsigned long lgA0PeriodMs();

void nastavMonitorPozastaven(bool pauza);
void prepniMonitorPozastaven();
void prepniSoloRezim();
void prepniDrzetStav();

void lgNastavDrzenyStav(uint8_t teplota, bool zapnuto);
void lgZrusCekaniOrig(const char* duvod);
void lgUkonciCekaniProStop();
void provedZapisTeploty(uint8_t teplota, bool zapnuto, bool zmenaStartu);

#endif
