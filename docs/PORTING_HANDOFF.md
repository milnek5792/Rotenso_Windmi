# LG Therma — shrnutí pro přenos na jiné tepelné čerpadlo

Stav vývoje k **2026-09-08** (commit `e3e08bf`, větev `fix/display-sleep-nvs`).  
Účel: podklad pro **nový repositář** a port ovládání na jinou značku TC.

Lokální ZIP záloha: `Documents/Arduino/backups/LG_Therma_20260908-082248.zip`.

---

## 1. Co systém dělá

Monorepo řídí tepelné čerpadlo z panelu **M5Stack Tab5** (ESP32-P4):

- čtení stavu a zápis povelů na sběrnici TC (dnes **LG Therma LIN**),
- pokojová / venkovní teplota (SwitchBot BLE přes C3 bridge),
- regulátor (ekviterm + PI) a týdenní plán,
- spotřeba (PZEM → ESP-NOW → bridge → Tab5),
- Wi‑Fi, MQTT, OTA, NVS historie.

**Produkční cesta hardwaru:** Tab5 + Xiao C3 bridge + ESP32-S3 PZEM.  
Env `waveshare-s3-7b` je volitelný / neúplný v tomto stromu.

---

## 2. Architektura (3 firmware)

```
┌─────────────────┐  LIN 300 baud    ┌──────────────┐
│ M5Stack Tab5    │◄────────────────►│ Tepelné čerp.│
│ ESP32-P4 + C6   │  UART1 RX38/TX37 │ (dnes LG)    │
└────────┬────────┘                  └──────────────┘
         │ UART 115200  G6/G7 ↔ C3 D6/D7
┌────────▼────────┐  ESP-NOW (stejný CH)  ┌──────────────────┐
│ XIAO ESP32-C3   │◄─────────────────────►│ ESP32-S3 Relay   │
│ BLE SwitchBot   │                       │ PZEM GPIO1/2     │
│ ESP-NOW RX      │                       │ Wi‑Fi STA vždy   │
└─────────────────┘                       └──────────────────┘
```

| Node | Env (PlatformIO) | Role |
|------|------------------|------|
| Tab5 | `m5stack-tab5-p4` / `-ota` | HMI, LIN, regulace, energie, MQTT |
| C3 | `bridge` / `bridge-ota` | BLE teploměry + ESP-NOW RX → UART |
| S3 | `esp32-s3-relay-pzem` / `-ota` | PZEM Modbus → ESP-NOW (+ OTA) |

Platforma: **pioarduino** Espressif32 **54.03.21**.

---

## 3. Datové toky

### Řízení TC
UI / MQTT / plán → `app_cmd` → `ui_bus_bindings` → **bus vrstva** (dnes `bus_lg_lin`) → sběrnice.  
Stav A0 → `bus_lg_model` → UI + regulátor.

### Teploty místnosti
SwitchBot BLE → C3 NimBLE → UART text → `climate_room_uart` → regulátor / UI.

### Spotřeba
PZEM → S3 (1 Hz, průměr 60 s) → ESP-NOW `PWR1`/`PIL1` → C3 → UART `PWR`/`PILOT` → `climate_energy` → graf Spotřeba + NVS.

### Sync Wi‑Fi kanálu (ESP-NOW)
Tab5 STA zjistí kanál AP → UART `WIFI CH=n` (≤ 5 min) → C3 nastaví rádio.  
S3 musí být **STA na stejném AP** (`peer.channel = 0`).

---

## 4. Softwarové vrstvy (klíčové cesty)

| Vrstva | Cesty | Přenositelnost |
|--------|-------|----------------|
| UI EEZ/LVGL | `ui_eez/`, `src/ui_eez_*`, `src/ui/ui_eez_*` | Vysoká (přejmenovat texty) |
| Příkazy | `src/app/app_cmd.*`, `ui_bus_bindings` | Vysoká — mění se jen backend |
| Regulátor | `src/climate/climate_regulator.*` | Vysoká |
| Plán | `src/climate/climate_plan.*` | Vysoká |
| Energie | `src/climate/climate_energy.*`, `ui_eez_energy` | Vysoká (bit aux = mapování značky) |
| Room UART | `climate_room_uart.*`, `h2_uart_protocol.h` | Vysoká |
| Bridge / PZEM | `h2_ble_bridge/`, `pzem_espnow/` | Vysoká |
| Net | `net_wifi_mgr`, `net_mqtt`, `net_ota`, NTP | Vysoká |
| NVS | `storage_config_nvs.*` | Vysoká (změnit namespace) |
| **Bus TC** | `bus_lg_lin.*`, `bus_lg_model.*`, `bus_lg_protocol.h` | **Nahradit** |

---

## 5. Co je specifické pro LG Therma (musí se vyměnit)

### LIN protokol (`bus_lg_*`)
- Rámce **20 B @ 300 baud**, checksum `sum ^ 0x55`.
- **A0** — stav venkovní jednotky (~25 s): teploty, B2/B3 bity.
- **C0** — povely: STOP `0x30`, status/SP/START `0x32`, B8 = SP vody °C.
- START sekvence: `32/02/00` → čekat A0 B3=`0x08` → `32/02/02`.
- Bity: čerpadlo `B2:0x02`, el. topení `B2:0x04`, běh `B3:0x08`, …
- Koexistence s originálním nástěnným ovladačem (SOLO / PARALLEL / DRŽET).

### Kosmetika / identifikátory
- NVS namespace `lg_therma`
- MQTT base / OTA hostnames `lgtherma-*`
- Názvy obrazovek, poruchy vázané na A0 kódy

### Cílové API pro novou značku
Zachovat rozhraní, které už UI a regulátor volají (přes `uiBus*` / model):

- start / stop,
- nastavit SP vody,
- číst vstup / výstup / SP / „žije“,
- příznaky: provoz, čerpadlo, kompresor, el. topení, porucha.

Implementovat jako `bus_<znacka>_*` a přepojit `ui_bus_bindings`.

---

## 6. Co znovupoužít beze změny logiky

1. **Regulátor** — ekviterm (−15…+15 °C) + PI (tick 120 s), Eco → SP 20 °C.  
2. **Týdenní plán** — VT1–4 + noc; NORMAL / útlum / VYP.  
3. **Spotřeba** — ΔkWh z Energy, týden minutových W, sezóna září–květen, 5 let.  
4. **C3 bridge** — BLE + ESP-NOW + UART protokol (`FOUND`, `PWR`, `PILOT`, `WIFI CH=`).  
5. **S3 PZEM** — Modbus Power/Energy, ESP-NOW, ArduinoOTA.  
6. **UI Spotřeba** — denní spojnicový graf (kW), měsíční bary, Y osa vázaná na LVGL mřížku.  
7. **MQTT / PWA** — `docs/pwa/` (volitelně přejmenovat topic base).

---

## 7. Konfigurace a NVS

**Compile-time (často tajemství — do gitu opatrně):**  
`wifi_config.h`, `mqtt_config.h`, `ble_config.h`, `espnow_energy_config.h`, OTA IP v `platformio.ini`.

**Runtime NVS (`lg_therma`):** Wi‑Fi, MQTT on/off, jas/sleep, plán, regulátor, režim UI, session TC, BLE MAC, energie (`en_meta`, `en_p0`…`en_p6`).

**Poznámka:** aktuální Wi‑Fi heslo je v lokálním `wifi_config.h` / ZIP; **není** v posledním gitu.

---

## 8. Provozní omezení (platí i po portu)

1. **S3 = vždy Wi‑Fi STA** — bez toho není ESP‑NOW kanál ani OTA.  
2. **C3 ne trvale na Wi‑Fi** — konflikt s BLE; OTA jen na povel, pak `WIFI OFF` + sync CH.  
3. **Kanál ESP‑NOW** — Tab5 sync `WIFI CH=` na C3; S3 bere CH z AP.  
4. **Energie NVS** — zápis příkonu ~30 min (první dirty ~45 s po bootu).  
5. **OTA IP** — Tab5 `.249`, bridge `.16`, PZEM `.98` (DHCP rezervace).  
6. **LIN task ≠ NVS** — flash jen z UI/ctrl cesty.  
7. **Tab5 SDIO** — Wi‑Fi C6 vs LVGL (arbiter).

---

## 9. Závislosti

- LVGL **9.2.2**, M5Unified/M5GFX (Tab5)  
- PubSubClient 2.8  
- NimBLE-Arduino (C3 / volitelně 7B)  
- ArduinoOTA / PlatformIO espota  
- EEZ Studio projekt v `ui_eez/`

---

## 10. Checklist nového repositáře

1. Fork / kopie stromu (bez `.pio`; secrets nově).  
2. Přejmenovat projekt, NVS ns, MQTT base, OTA hostnames, MQTT/PWA topic.  
3. **Nahradit `bus_lg_*`** implementací sběrnice nové značky se stejným `uiBus*` API.  
4. Přemapovat status bity (provoz, čerpadlo, aux, poruchy) → model / el. topení 3 kW.  
5. Nechat climate / energy / bridge / PZEM / UI; upravit texty a poruchy.  
6. Nové Wi‑Fi/MQTT/BLE MAC/ESP‑NOW peer MAC; OTA IP.  
7. Ověřit: START/STOP/SP, regulátor, plán, BLE teploty, ESP‑NOW + `WIFI CH=`, Spotřeba po rebootu.  
8. První commit v novém repu = „baseline from LG_Therma e3e08bf“ + tento dokument.

---

## 11. Doporučená struktura nového repa

```
<Brand>_HeatPump/
  docs/PORTING_HANDOFF.md   ← tento soubor (upravený)
  platformio.ini            ← envs: panel / bridge / pzem
  include/                  ← config bez LG jmen
  src/
    bus_<brand>/            ← NOVÁ sběrnice
    climate/                ← z LG Therma
    ui/ …
  h2_ble_bridge/            ← z LG Therma (příp. přejmenovat)
  pzem_espnow/
  ui_eez/
```

---

## 12. Stav založení nového repa

**Hotovo (2026-09-08):**

1. Lokální cesta: `C:\Users\mnekv\Documents\Arduino\Rotenso_Windmi`
2. Baseline z LG_Therma `e3e08bf` (bez `.pio`, bez reálných Wi‑Fi hesel)
3. První commit: baseline import + `docs/PORTING_HANDOFF.md`
4. Secrets: `include/wifi_config.h` / `mqtt_config.h` v `.gitignore`; v gitu jen `*.example.h`

**Zbývá:** GitHub `milnek5792/Rotenso_Windmi` + `git push -u origin main`.

---

## 13. Cíl nového projektu: Rotenso Windmi

**Rozhodnuto (2026-09-08):**

| Položka | Volba |
|---------|--------|
| Značka / řada | **Rotenso Windmi** |
| Sběrnice TC | **Modbus RTU po RS485** (Tab5 = master) |
| Panel | **M5Stack Tab5** zůstává |
| Bridge C3 | **Zůstává v architektuře**; finální nutnost **ještě otevřená** |
| PZEM / energie | Stejný model (S3 + ESP-NOW), pokud se C3 ponechá |

### Modbus Windmi — výchozí parametry (z manuálu / HA komunity)

- Typ: **Modbus RTU**
- Baud: **9600** (konfigurovatelné)
- Slave adresa: **11** (konfigurovatelné)
- Rámec: **8N1** (konfigurovatelné)
- Registr mapa: kapitola Modbus v Installation & User Manual (Windmi Series); praktický přehled čtení např. [HA gist hvdb](https://gist.github.com/hvdb/a6a6fdc889573084ac2bdd53e71303c7) (adresy typu Setting Mode 44, Running Mode 45, Occupancy 41, frekvence kompresoru 23, … — ověřit proti oficiální tabulce před zápisem).

### Hardware sběrnice na Tab5

- Tab5 má **vestavěný RS485** (SIT3088 + přepínatelný 120 Ω terminátor).
- Piny ESP32-P4 ([m5-docs PinMap](https://docs.m5stack.com/en/core/Tab5)):

| Funkce | GPIO |
|--------|------|
| RS485 TX | **G20** |
| RS485 RX | **G21** |
| RS485 DIR (DE/RE) | **G34** |

- LG LIN dnes běží na **jiném** UART (RX38/TX37 @ 300 baud) — pro Windmi se **nepoužije**.
- Tab5 = **Modbus master**; Windmi = slave (výchozí adresa 11). Externí RS485 modul **není potřeba**.

### Softwarový šev (nový repo)

```
uiBus* / climate_regulator / plán
        │
        ▼
  bus_rotenso_modbus.cpp   ← NOVÉ (místo bus_lg_lin)
        │
        ▼
  UART G20/G21 + DIR G34 (SIT3088)  →  Windmi slave :11
```

Mapovat registry → stejné UI pojmy: START/STOP, SP vody, T in/out, provoz, čerpadlo, el. dohřev, porucha.

### Role C3 — rozhodovací matice

| Funkce | Bez C3 | S C3 |
|--------|--------|------|
| SwitchBot BLE teploměry | Nutný jiný BLE zdroj (Tab5 C6 BLE? / jiný bridge) | Ano, jako teď |
| ESP-NOW od PZEM S3 | Přímý příjem na Tab5 (C6 Wi‑Fi) — ověřit koexistenci s STA/MQTT | Ano, osvědčené |
| UART diagnostika bridge | — | Zachovat |

**Doporučení do rozhodnutí:** v novém repu **C3 zatím ponechat** (kopie `h2_ble_bridge`); pokud později Tab5 zvládne BLE + ESP-NOW na C6 bez nestability, C3 vyřadit feature-flagem.

### Název nového repa (návrh)

`Rotenso_Windmi` / `Windmi_Tab5` — bez `LG_Therma` v názvu; NVS ns např. `windmi`, OTA `windmi-tab5`, MQTT base `windmi/...`.

### První milníky nového repa

1. Import baseline z LG Therma + tento dokument.  
2. Vestavěný RS485 Tab5 + Modbus RTU poll (read-only stav).  
3. Zápis START/STOP + SP vody.  
4. Napojení regulátoru / plánu.  
5. C3/BLE + energie (nebo plán B bez C3).  
6. Ověření proti reálné jednotce Windmi.
