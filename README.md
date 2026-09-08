# Rotenso Windmi

Tab5 control for Rotenso Windmi (Modbus RTU / RS485).

Baseline imported from LG_Therma (`e3e08bf`). See `docs/PORTING_HANDOFF.md`.

## Setup

1. Copy `include/wifi_config.example.h` → `include/wifi_config.h` and set SSID/password.
2. Copy `include/mqtt_config.example.h` → `include/mqtt_config.h` if needed.
3. Build: `pio run -e m5stack-tab5-p4`
