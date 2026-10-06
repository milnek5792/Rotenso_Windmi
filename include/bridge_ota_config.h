// bridge_ota_config.h — ArduinoOTA na Xiao C3 BLE bridge
#ifndef BRIDGE_OTA_CONFIG_H
#define BRIDGE_OTA_CONFIG_H

/** Hostname prefix — doplní se MAC, např. windmi-bridge-A1B2C3 */
#ifndef BRIDGE_OTA_HOST_PREFIX
#define BRIDGE_OTA_HOST_PREFIX "windmi-bridge"
#endif

/** Volitelné heslo espota (prázdné = bez hesla). */
#ifndef BRIDGE_OTA_PASSWORD
#define BRIDGE_OTA_PASSWORD ""
#endif

/** Po nečinnosti vypnout Wi‑Fi (ms). */
#ifndef BRIDGE_OTA_IDLE_MS
#define BRIDGE_OTA_IDLE_MS (15UL * 60UL * 1000UL)
#endif

#endif
