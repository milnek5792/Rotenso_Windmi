// bus_rotenso_modbus.h — Windmi Modbus RTU (FC 0x04 read / 0x06 write)
#ifndef BUS_ROTENSO_MODBUS_H
#define BUS_ROTENSO_MODBUS_H

#include <Arduino.h>

#ifdef __cplusplus
extern "C" {
#endif

void rotensoBusInit(void);
void rotensoBusTick(void);
bool rotensoBusIsReady(void);

unsigned long rotensoBusOkCount(void);
unsigned long rotensoBusFailCount(void);

void rotensoBusSetConfigScreenActive(bool active);
bool rotensoBusQueueConfigWrite(uint16_t addr, uint16_t value);
void rotensoBusRequestConfigPoll(void);

#ifdef __cplusplus
}
#endif

#endif
