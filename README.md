# Rotenso Windmi

Ovládání tepelného čerpadla **Rotenso Windmi** z **M5Stack Tab5** (Modbus RTU / RS485) + BLE pokoj, PZEM spotřeba, MQTT a PWA.

## Dokumentace

| Soubor | Obsah |
|--------|--------|
| [docs/PROJECT.md](docs/PROJECT.md) | Architektura, MQTT API, build, OTA, pastí |
| [docs/CREDENTIALS.local.md](docs/CREDENTIALS.local.md) | **Servery a hesla** (jen lokálně, gitignored) |
| [docs/PORTING_HANDOFF.md](docs/PORTING_HANDOFF.md) | Historie portu z LG Therma |

## Setup

1. Zkopíruj `include/wifi_config.example.h` → `include/wifi_config.h` (SSID/heslo).
2. Zkopíruj `include/mqtt_config.example.h` → `include/mqtt_config.h` (EMQX).
3. Volitelně vyplň `docs/CREDENTIALS.local.md` podle šablony v gitu / lokální kopie.
4. Build Tab5: `pio run -e m5stack-tab5-p4`
5. OTA Tab5: `pio run -e m5stack-tab5-ota -t upload`

## PWA

Zdroj: `docs/pwa/` → GitHub Pages (viz PROJECT.md). Po změně JS bump SW cache.

## Android APK

PWA je zabalená přes **Capacitor** (`android-app/`) jako WebView na GitHub Pages URL.

- Workflow: `.github/workflows/build-apk.yml` (Actions → **Build APK** → artifact `WindmiCtrl-apk`)
- Spuštění: push do `android-app/**`, manuálně **workflow_dispatch**, nebo GitHub Release (přiloží APK)
- Sideload: stáhni `WindmiCtrl-latest.apk` z artifactu / release a nainstaluj (povolit neznámé zdroje)

## Hardware (zkráceně)

- **Tab5** — HMI + Modbus RS485 + MQTT  
- **XIAO C3** — SwitchBot BLE + ESP-NOW RX  
- **ESP32-S3** — PZEM → ESP-NOW  

Detail a credentials: `docs/PROJECT.md` + `docs/CREDENTIALS.local.md`.
