# Rotenso Windmi — dokumentace projektu

Stav k **2026-10-10**. Monorepo řídí tepelné čerpadlo **Rotenso Windmi** z panelu **M5Stack Tab5**.

**Hesla a servery s credentials:** soubor [`CREDENTIALS.local.md`](CREDENTIALS.local.md) (gitignored — jen lokálně).  
Starší přenosový popis LG→Windmi: [`PORTING_HANDOFF.md`](PORTING_HANDOFF.md).

---

## 1. Přehled

Systém umí:

- číst stav TČ a zapisovat provoz přes **Modbus RTU (RS485)**;
- ovládat START/STOP (režim **Heat**), setpoint vody / pokoje;
- **regulátor** (ekviterm + pokojový PI) a **týdenní plán** (včetně útlumu SP);
- pokojovou teplotu ze **SwitchBot Meter** (BLE přes Xiao C3);
- spotřebu z **PZEM** (ESP32-S3 → ESP-NOW → C3 → Tab5);
- **Wi‑Fi, MQTT (EMQX), OTA, PWA** na mobilu.

---

## 2. Hardwarová architektura

```
┌──────────────────────┐   Modbus RTU 9600   ┌─────────────────┐
│ M5Stack Tab5         │◄───────────────────►│ Rotenso Windmi  │
│ ESP32-P4 + C6 Wi‑Fi  │   RS485 G20/21/34   │ Slave ID 11     │
│ 1280×720 LVGL/EEZ    │                     └─────────────────┘
└──────────┬───────────┘
           │ UART 115200  Tab5 G6/G7 ↔ C3 D7/D6
┌──────────▼───────────┐   ESP-NOW (stejný CH)  ┌────────────────────┐
│ Seeed XIAO ESP32-C3  │◄─────────────────────►│ ESP32-S3 Relay      │
│ BLE SwitchBot        │                       │ PZEM GPIO1/2        │
│ ESP-NOW RX → UART    │                       │ Wi‑Fi STA (kanál AP)│
└──────────────────────┘                       └────────────────────┘
           │
           ▼ MQTTS / WSS
    EMQX Cloud Serverless
           │
           ▼
    PWA (GitHub Pages) / MQTTX / HA
```

| Node | PlatformIO env | Role |
|------|----------------|------|
| Tab5 | `m5stack-tab5-p4` / `m5stack-tab5-ota` | HMI, Modbus, regulace, MQTT, plán |
| C3 | `bridge` / `bridge-ota` | BLE + ESP-NOW RX → UART na Tab5 |
| S3 | `esp32-s3-relay-pzem` / `-ota` | PZEM → ESP-NOW (+ OTA) |

Platforma: **pioarduino** Espressif32 **54.03.21**.

---

## 3. Softwarové vrstvy (Tab5)

| Vrstva | Cesty | Poznámka |
|--------|-------|----------|
| UI | `src/ui_eez_*`, `src/ui/ui_eez_*`, `ui_eez/` | LVGL 9 + EEZ |
| Dotyk | `src/ui_touch_tab5.cpp` | custom hit-test (SDIO) |
| Příkazy | `src/app/app_cmd.*` | fronta HMI/MQTT |
| Bus bindings | `src/ui/ui_bus_bindings.cpp` | START/STOP, SP, režimy |
| Modbus | `src/bus/bus_rotenso_modbus.*` | FC 0x04 / 0x06 |
| Model | `src/bus_lg_model.*` | live snap, session NVS |
| Regulátor | `src/climate/climate_regulator.*` | ekviterm + PI, plánový offset |
| Plán | `src/climate/climate_plan.*` | NORMAL / ÚTLUM / VYP |
| Pokoj | `src/climate_room_uart.*` | text UART z C3 |
| Energie | `src/climate/climate_energy.*` | PZEM + IBH |
| Síť | `src/net_wifi_mgr.*`, `src/net_mqtt_client.*`, `src/net_ota.*` | Wi‑Fi / MQTT / OTA |
| NVS | `storage_config_nvs.*` | session, SP, plán, energie |

---

## 4. Modbus Windmi (shrnutí)

Konfigurace: `include/bus_rotenso_config.h`.

| Položka | Hodnota |
|---------|---------|
| Baud | 9600 8N1 |
| Slave | 11 |
| Live read | FC **0x04** Input Registers |
| Config RW | FC **0x03** / write **0x06** Holding |
| RS485 | TX=20, RX=21, DIR=34 |

Důležité registry:

| Reg | Význam |
|-----|--------|
| 0001–0004 | Teploty (°C×10) |
| **002C** | Setting mode (zápis Heat=2 / Off=0) |
| **002D** | Running mode (čtení — potvrzení CHOD) |
| 0191 | Water setpoint (°C×10) |
| 0055 | Pump speed |
| 0017 | Comp frequency |
| 0209 | UI type (1=kontakty, 2=WUI) |

**START** musí zapsat `002C = Heat (2)`. Nesmí se zrušit zápisem SP vody (`pozadavekZmenaStartu`).  
**CHOD** na UI = potvrzený `running mode != 0` (002DH). Signálka čerpadla = pump/flow/run.

---

## 5. Regulace a plán

### Režimy UI (`uiEez.rezim`)

| Režim | MQTT `tele/reg_mode` | Chování |
|-------|----------------------|---------|
| Pokoj PI | `room` | SP pokoje 18–24 °C (0,5 °C), voda píše regulátor |
| Ekviterm | `equitherm` | SP vody = křivka + korekce |
| Ruční | `water` | přímý SP vody 25–63 °C |

### Plánový útlum

- V pokoji: `room_sp_effective = room_sp + plan_offset` (offset typicky −1…−5 °C).
- Tab i PWA ukazují **effective** SP.
- PWA posílá jen `cmd/setpoint` = `+` / `-` (ne absolutní hodnotu), aby se útlum nezapsal jako nový základ.

---

## 6. MQTT API (`windmi/…`)

### Telemetrie (retained, při aktivním watch)

| Topic | Obsah |
|-------|--------|
| `availability` | `online` / LWT |
| `tele/temp_room` | pokoj °C |
| `tele/temp_outdoor` | venku (z TČ) |
| `tele/temp_inlet` / `outlet` | voda |
| `tele/temp_set` | SP (effective v room) |
| `tele/reg_mode` | `room` / `equitherm` / `water` |
| `tele/eq_offset` | korekce ekvitermy |
| `tele/power` | ON/OFF (CHOD) |
| `tele/pump` | čerpadlo |
| `tele/compressor` | kompresor |
| `tele/lin` | sběrnice live (název historický) |
| `tele/watch` | rychlá tele aktivní |
| `tele/alarm` / `tele/porucha` | poruchy |

### Příkazy

| Topic | Payload |
|-------|---------|
| `cmd/watch` | `ON` / `OFF` — nutné pro rychlou tele |
| `cmd/power` | `ON` / `OFF` — START Heat / STOP |
| `cmd/setpoint` | `+` / `-` (room = ±0,5 °C); volitelně absolutní |
| `cmd/mode` | `room` / `equitherm` / `water` |

PWA nejdřív pošle `watch`, pak příkaz. Tab frontuje RX mimo MQTT callback (PubSubClient reentrancy).

---

## 7. PWA

| Položka | Hodnota |
|---------|---------|
| Zdroj | `docs/pwa/` |
| URL | viz `CREDENTIALS.local.md` (GitHub Pages) |
| SW cache | `docs/pwa/sw.js` (`windmi-pwa-w2-v…`) |
| Cache bust | `?v=w2…` v `index.html` / importech |

Po změně JS: bump SW + query string, hard refresh / `reset.html`.

---

## 8. Build a upload

```bash
# Tab5 USB
pio run -e m5stack-tab5-p4 -t upload

# Tab5 OTA (IP v platformio.ini)
pio run -e m5stack-tab5-ota -t upload

# C3 bridge
pio run -e bridge -t upload
pio run -e bridge-ota -t upload

# S3 PZEM
pio run -e esp32-s3-relay-pzem -t upload
pio run -e esp32-s3-relay-pzem-ota -t upload
```

Lokální secrets (ne v gitu):

1. `include/wifi_config.h` ← z `wifi_config.example.h`
2. `include/mqtt_config.h` ← z `mqtt_config.example.h`

FW verze: `include/app_build_stamp.h` (generuje `scripts/gen_build_stamp.py`).

---

## 9. Síť a servery (bez hesel)

| Služba | Účel |
|--------|------|
| Domácí Wi‑Fi | Tab5 / bridge OTA / PZEM STA |
| EMQX Cloud (EU) | MQTTS 8883 + WSS 8084 |
| GitHub Pages | hosting PWA |
| NTP | `pool.ntp.org`, Cloudflare, europe pool |
| PlatformIO registry / GitHub | knihovny (M5, LVGL, PubSubClient, NimBLE) |

Konkrétní hostnames, IP, user/password → **`docs/CREDENTIALS.local.md`**.

---

## 10. Důležité chování / pastí

1. **START + ImmediateTick** — zápis SP nesmí shodit `pozadavekZmenaStartu` (Heat 002C).
2. **SDIO / NVS** — flash zápis během Wi‑Fi/MQTT může shodit Tab5; room SP z MQTT bez ImmediateTick.
3. **PubSubClient** — publish v callbacku = ztráta cmd; RX queue + drain mimo callback.
4. **PWA absolutní SP + plánový útlum** — dříve kazilo základ SP; proto jen `+/-`.
5. **Font cs_24** — UI texty: raději ASCII pro `—` `→` `°` (bridge/regulátor).

---

## 11. Zálohy

| Typ | Kde |
|-----|-----|
| ZIP | `C:\Users\mnekv\Documents\Arduino\backups\Rotenso_Windmi_*.zip` |
| Git tag | např. `backup-2026-10-10-ui-text`, `backup-2026-10-09-pwa-mqtt` |
| Remote | `origin/main` na GitHub |

Po větší změně: ZIP + commit + annotated tag + `docs/CREDENTIALS.local.md` na bezpečné místo.

---

## 12. Struktura repa (zkráceně)

```
Rotenso_Windmi/
  include/          # config (wifi/mqtt lokálně gitignored)
  src/              # Tab5 firmware
  h2_ble_bridge/    # C3
  pzem_espnow/      # S3
  docs/pwa/         # PWA
  docs/PROJECT.md   # tato dokumentace
  docs/CREDENTIALS.local.md  # hesla (gitignore)
  platformio.ini
```

---

## 13. Rychlá diagnostika

| Symptom | Co zkontrolovat |
|---------|-----------------|
| PWA tlačítka nic | MQTT připojení, `cmd/watch`, hard refresh SW |
| START jen LED, ne Heat | Serial `[WM] WR MODE 2`, pending Start vs SP |
| SP PWA ≠ Tab | útlum plánu (effective), tele/temp_set |
| Bridge čtverečky | speciální unicode v textech |
| OTA Listen Failed | `host_port=8266` v `platformio.ini` |
