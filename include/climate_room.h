// climate_room.h — sjednocené API pokojové teploty (BLE/H2)
// Venkovní teplota = Modbus TČ (mVenkovniC / uiEez.teplota_venkovni).
#ifndef CLIMATE_ROOM_H
#define CLIMATE_ROOM_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void climateRoomInit(void);
void climateRoomTick(void);
void climateRoomRequestNow(void);

bool climateRoomIsOk(void);
bool climateRoomIsBusy(void);
void climateRoomReleaseForTls(void);
bool climateRoomBootPollPending(void);

float climateRoomTempC(void);
float climateRoomHumidity(void);
int climateRoomBatteryPct(void);
int climateRoomRssi(void);

void climateRoomStatusText(char* buf, size_t buflen);

#ifdef __cplusplus
}
#endif

#endif
