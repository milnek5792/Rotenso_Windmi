// espnow_energy_config.h — ESP-NOW paket spotřeby (S3 PZEM ↔ C3 bridge)
#ifndef ESPNOW_ENERGY_CONFIG_H
#define ESPNOW_ENERGY_CONFIG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Magic 'PWR1' — minutový vzorek spotřeby */
#ifndef ESPNOW_ENERGY_MAGIC
#define ESPNOW_ENERGY_MAGIC 0x31525750u
#endif

/** Magic 'PIL1' — krátký pilot (rádio žije), před PWR / periodicky */
#ifndef ESPNOW_PILOT_MAGIC
#define ESPNOW_PILOT_MAGIC 0x314C4950u
#endif

/**
 * Výchozí Wi‑Fi kanál ESP-NOW na C3 (boot / než Tab5 pošle `WIFI CH=`).
 * Za běhu Tab5 syncuje kanál AP; S3 bere kanál ze STA.
 */
#ifndef ESPNOW_ENERGY_CHANNEL
#define ESPNOW_ENERGY_CHANNEL 11
#endif

/**
 * MAC peeru C3 bridge (STA). Přepiš build flagem / úpravou po zjištění MAC.
 * Formát: šest bajtů.
 */
#ifndef ESPNOW_ENERGY_PEER_MAC0
#define ESPNOW_ENERGY_PEER_MAC0 0x1C
#define ESPNOW_ENERGY_PEER_MAC1 0xDB
#define ESPNOW_ENERGY_PEER_MAC2 0xD4
#define ESPNOW_ENERGY_PEER_MAC3 0xF0
#define ESPNOW_ENERGY_PEER_MAC4 0xC7
#define ESPNOW_ENERGY_PEER_MAC5 0x58
#endif

/** Flag: Energy na PZEM byla v této minutě resetována. */
#define ESPNOW_ENERGY_FLAG_RESET 0x01u
/** Flag: aspoň jeden platný Modbus read v okně (měřič komunikuje). */
#define ESPNOW_ENERGY_FLAG_PZEM_OK 0x02u

/** Minimální velikost starého PWR paketu (bez diag počítadel). */
#define ESPNOW_ENERGY_PKT_MIN_SIZE 12

#pragma pack(push, 1)
typedef struct {
  uint32_t magic;
  uint16_t avg_power_w;
  uint32_t energy_wh;
  uint8_t flags;
  uint8_t seq;
  /** Úspěšné / neúspěšné PZEM čtení za poslední minutu (0..255). */
  uint8_t pzem_ok_cnt;
  uint8_t pzem_fail_cnt;
} EspNowEnergyPacket;

/** Lehký heartbeat — důkaz, že S3 vysílá (nezávisle na platném PWR). */
typedef struct {
  uint32_t magic;
  uint8_t flags;       // ESPNOW_ENERGY_FLAG_PZEM_OK = poslední sample OK
  uint8_t seq;
  uint8_t pzem_live;   // 1 = poslední Modbus read OK, 0 = fail
  uint8_t _pad;
} EspNowPilotPacket;
#pragma pack(pop)

#ifdef __cplusplus
}
#endif

#endif
