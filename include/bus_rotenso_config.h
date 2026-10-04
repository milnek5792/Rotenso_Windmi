// bus_rotenso_config.h — Rotenso Windmi Modbus RTU (Tab5 RS485)
#ifndef BUS_ROTENSO_CONFIG_H
#define BUS_ROTENSO_CONFIG_H

#include "lg_board.h"

/** Tab5 RS485 (SIT3088): TX G20, RX G21, DIR/DE-RE G34 */
#ifndef WINDMI_RS485_TX_PIN
#define WINDMI_RS485_TX_PIN 20
#endif
#ifndef WINDMI_RS485_RX_PIN
#define WINDMI_RS485_RX_PIN 21
#endif
#ifndef WINDMI_RS485_DIR_PIN
#define WINDMI_RS485_DIR_PIN 34
#endif

/** Tab5 RS485 = UART1 / Serial1 */
#ifndef WINDMI_UART_NUM
#define WINDMI_UART_NUM 1
#endif

/** Manuál: 9600 8N1, slave 11 */
#ifndef WINDMI_MB_BAUD
#define WINDMI_MB_BAUD 9600
#endif
#ifndef WINDMI_MB_SLAVE
#define WINDMI_MB_SLAVE 11
#endif

/**
 * Pevný protokol — žádná autodetect:
 *   čtení  = FC 0x04 (Input Registers)
 *   zápis  = FC 0x06 (Write Single Register)
 */
#ifndef WINDMI_FC_READ
#define WINDMI_FC_READ 0x04
#endif
#ifndef WINDMI_FC_WRITE
#define WINDMI_FC_WRITE 0x06
#endif

/** Teploty 0001H…0004H (°C×10, −40 = neplatné) */
#ifndef WINDMI_REG_TEMP_BASE
#define WINDMI_REG_TEMP_BASE 0x0001
#endif
#ifndef WINDMI_REG_TEMP_COUNT
#define WINDMI_REG_TEMP_COUNT 4
#endif
#ifndef WINDMI_REG_OUTDOOR
#define WINDMI_REG_OUTDOOR 0x0001
#endif
#ifndef WINDMI_REG_TW_IN
#define WINDMI_REG_TW_IN 0x0003
#endif
#ifndef WINDMI_REG_T1_OUT
#define WINDMI_REG_T1_OUT 0x0004
#endif
#ifndef WINDMI_TEMP_SCALE
#define WINDMI_TEMP_SCALE 0.1f
#endif
#ifndef WINDMI_TEMP_INVALID_C
#define WINDMI_TEMP_INVALID_C (-40.0f)
#endif
#ifndef WINDMI_TEMP_INVALID_RAW
#define WINDMI_TEMP_INVALID_RAW (-400)
#endif

/** 002CH Setting mode, 002DH Running mode */
#ifndef WINDMI_REG_SETTING_MODE
#define WINDMI_REG_SETTING_MODE 0x002C
#endif
#ifndef WINDMI_REG_RUNNING_MODE
#define WINDMI_REG_RUNNING_MODE 0x002D
#endif
#ifndef WINDMI_REG_COMP_FREQ
#define WINDMI_REG_COMP_FREQ 0x0017
#endif
#ifndef WINDMI_REG_PUMP_SPEED
#define WINDMI_REG_PUMP_SPEED 0x0055
#endif
#ifndef WINDMI_REG_QUIET_NIGHT
#define WINDMI_REG_QUIET_NIGHT 0x0044
#endif
#ifndef WINDMI_REG_WATER_FLOW
#define WINDMI_REG_WATER_FLOW 0x102A
#endif
#ifndef WINDMI_RUN_DEFROST
#define WINDMI_RUN_DEFROST 7
#endif
#ifndef WINDMI_SET_OFF
#define WINDMI_SET_OFF 0
#endif
#ifndef WINDMI_SET_HEAT
#define WINDMI_SET_HEAT 2
#endif
#ifndef WINDMI_REG_LOAD_OUTPUT
#define WINDMI_REG_LOAD_OUTPUT 0x0081
#endif
#ifndef WINDMI_LOAD_IBH1_BIT
#define WINDMI_LOAD_IBH1_BIT 0x0001u
#endif

#ifndef WINDMI_MB_GAP_MS
#define WINDMI_MB_GAP_MS 50u
#endif

#ifndef WINDMI_REG_WATER_SP
#define WINDMI_REG_WATER_SP 0x0191
#endif
#ifndef WINDMI_REG_WATER_CTRL
#define WINDMI_REG_WATER_CTRL 0x0033
#endif
#ifndef WINDMI_REG_TW_OUT
#define WINDMI_REG_TW_OUT 0x1008
#endif
#ifndef WINDMI_WATER_SP_MIN_C
#define WINDMI_WATER_SP_MIN_C 25
#endif
#ifndef WINDMI_WATER_SP_MAX_C
#define WINDMI_WATER_SP_MAX_C 63
#endif

#ifndef WINDMI_REG_CTRL_MODE
#define WINDMI_REG_CTRL_MODE 0x100D
#endif
#ifndef WINDMI_CTRL_WATER
#define WINDMI_CTRL_WATER 0
#endif
#ifndef WINDMI_CTRL_AMBIENT
#define WINDMI_CTRL_AMBIENT 1
#endif
/** 0209H ui_type: 0=bez ovladače, 1=kontakty, 2=drátový WUI */
#ifndef WINDMI_REG_UI_TYPE
#define WINDMI_REG_UI_TYPE 0x0209
#endif
#ifndef WINDMI_UI_NONE
#define WINDMI_UI_NONE 0
#endif
#ifndef WINDMI_UI_CONTACTS
#define WINDMI_UI_CONTACTS 1
#endif
#ifndef WINDMI_UI_WIRED
#define WINDMI_UI_WIRED 2
#endif
#ifndef WINDMI_REG_CURVE_TYPE
#define WINDMI_REG_CURVE_TYPE 0x0245
#endif
#ifndef WINDMI_CURVE_FIXED_SP
#define WINDMI_CURVE_FIXED_SP (-1)
#endif
#ifndef WINDMI_REG_BACKUP_HEATER
#define WINDMI_REG_BACKUP_HEATER 0x0259
#endif
#ifndef WINDMI_BACKUP_INNER_EH
#define WINDMI_BACKUP_INNER_EH 6
#endif
#ifndef WINDMI_REG_MIN_OAT_HEAT
#define WINDMI_REG_MIN_OAT_HEAT 0x0202
#endif
#ifndef WINDMI_REG_IBH_WARMUP
#define WINDMI_REG_IBH_WARMUP 0x025A
#endif
#ifndef WINDMI_REG_IBH_DELTA_T
#define WINDMI_REG_IBH_DELTA_T 0x025B
#endif
#ifndef WINDMI_REG_IBH_OAT
#define WINDMI_REG_IBH_OAT 0x025C
#endif
#ifndef WINDMI_REG_PUMP_DELTA_T
#define WINDMI_REG_PUMP_DELTA_T 0x0239
#endif
#ifndef WINDMI_REG_REQ_COMP_FREQ
#define WINDMI_REG_REQ_COMP_FREQ 0x100F
#endif
/** Alarm bitmapy #1…#4 (1009H…100CH), FC 0x04 */
#ifndef WINDMI_REG_ALARM_BM1
#define WINDMI_REG_ALARM_BM1 0x1009
#endif
#ifndef WINDMI_REG_ALARM_COUNT
#define WINDMI_REG_ALARM_COUNT 4
#endif

/** @deprecated — nepoužívat; čtení je vždy WINDMI_FC_READ */
#ifndef WINDMI_PROBE_FC
#define WINDMI_PROBE_FC WINDMI_FC_READ
#endif

#ifndef WINDMI_POLL_TEMP_MS
#define WINDMI_POLL_TEMP_MS 0u
#endif
#ifndef WINDMI_POLL_STATUS_MS
#define WINDMI_POLL_STATUS_MS 0u
#endif
#ifndef WINDMI_POLL_CONFIG_MS
#define WINDMI_POLL_CONFIG_MS 0u
#endif
#ifndef WINDMI_POLL_MS
#define WINDMI_POLL_MS 0u
#endif

#ifndef WINDMI_MB_TIMEOUT_MS
#define WINDMI_MB_TIMEOUT_MS 500u
#endif
/** Po flush TX ≥1 znak @9600 než DIR→RX */
#ifndef WINDMI_MB_TX_SETTLE_US
#define WINDMI_MB_TX_SETTLE_US 1500u
#endif

#if !LG_BOARD_TAB5
#warning "bus_rotenso: RS485 piny jsou pro Tab5; na 7B uprav WINDMI_RS485_*"
#endif

#endif
