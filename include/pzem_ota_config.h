// pzem_ota_config.h — ArduinoOTA pro Waveshare ESP32-S3-Relay (PZEM)
#ifndef PZEM_OTA_CONFIG_H
#define PZEM_OTA_CONFIG_H

#ifndef PZEM_OTA_HOSTNAME
#define PZEM_OTA_HOSTNAME "lgtherma-pzem"
#endif

/** Volitelné heslo espota (prázdné = bez hesla). */
#ifndef PZEM_OTA_PASSWORD
#define PZEM_OTA_PASSWORD ""
#endif

/** Timeout jednoho pokusu připojení Wi‑Fi (ms). */
#ifndef PZEM_WIFI_CONNECT_MS
#define PZEM_WIFI_CONNECT_MS 20000u
#endif

/** Pauza mezi reconnect pokusy (ms). */
#ifndef PZEM_WIFI_RETRY_MS
#define PZEM_WIFI_RETRY_MS 15000u
#endif

#endif
