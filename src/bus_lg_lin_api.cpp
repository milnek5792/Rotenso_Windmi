// bus_lg_lin_api.cpp — stuby po odstranění LG LIN (Windmi = jen Modbus)
#include "bus_lg_lin_api.h"

#include "bus_lg_model.h"
#include "storage_config_nvs.h"

#include <Arduino.h>

void lgBusInit() {}
void lgBusTick() {}

unsigned long lgPocetPaketu() { return 0; }
unsigned long lgPocetRxBajtu() { return 0; }
bool lgBusIsReady() { return false; }
bool lgZapisBezi() { return false; }

unsigned long lgA0PeriodMs() { return 0; }

void nastavMonitorPozastaven(bool pauza) { monitorPozastaven = pauza; }
void prepniMonitorPozastaven() { monitorPozastaven = !monitorPozastaven; }

void prepniSoloRezim() {
  soloRezimTab5 = !soloRezimTab5;
}

void prepniDrzetStav() {
  parallelRezimTab5 = !parallelRezimTab5;
  if (!parallelRezimTab5) {
    drzetStavAktivni = false;
  }
}

void lgNastavDrzenyStav(uint8_t teplota, bool zapnuto) {
  cilovaTeplotaTab5 = teplota;
  cilovyZapnutoTab5 = zapnuto;
  drzetStavAktivni = true;
  storageRequestSaveTcSession(zapnuto, teplota);
}

void lgZrusCekaniOrig(const char* /*duvod*/) {
  cekameNaOrigStart = false;
  tcPozadavekZap = false;
  drzetStavAktivni = false;
}

void lgUkonciCekaniProStop() {
  cekameNaOrigStart = false;
}

void provedZapisTeploty(uint8_t teplota, bool zapnuto, bool zmenaStartu) {
  // Modbus task čte tyto flagy v processOneWrite()
  lgModelLock();
  novaCilovaTeplota = teplota;
  pozadavekZmenaStartu = zmenaStartu;
  pozadavekNaZapis = true;
  if (zmenaStartu) {
    cilovyZapnutoTab5 = zapnuto;
    tcPozadavekZap = zapnuto;
    mMbPowerPending = true;
    mMbPowerWantOn = zapnuto;
  }
  lgModelUnlock();
}
