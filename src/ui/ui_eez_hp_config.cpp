#include "ui_eez_hp_config.h"

#include "lg_board.h"
#include "app_cmd.h"
#include "bus_rotenso_config.h"
#include "src/bus/bus_rotenso_modbus.h"
#include "src/bus_lg_model.h"
#include "src/ui_eez_fonts.h"
#include "src/ui_eez_nav.h"

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

hp_config_objects_t hpConfigObj;

namespace {

constexpr uint32_t kColBg = 0x121214u;
constexpr uint32_t kColPanel = 0x1A1A1Fu;
constexpr uint32_t kColBorder = 0x24242Bu;
constexpr uint32_t kColText = 0xE0E0E6u;
constexpr uint32_t kColMuted = 0x8E8E93u;
constexpr uint32_t kColAccent = 0x0A84FFu;
constexpr uint32_t kColOrange = 0xFF9F0Au;
constexpr uint32_t kColGreen = 0x30D158u;
constexpr uint32_t kColPurple = 0x5856D6u;

constexpr int kW = BOARD_PANEL_W;
constexpr int kH = BOARD_PANEL_H;
constexpr int kMargin = 10;
constexpr int kGap = 8;
constexpr int kBtnH = 44;
constexpr int kPad = 10;
constexpr int kRowH = 48;
constexpr int kSmallBtnW = 56;

const lv_font_t* kFont = &ui_font_font_cs_24;
const lv_font_t* kFontTitle = &ui_font_font_cs_28;
bool s_created = false;
bool s_active = false;

void setLabelIfChanged(lv_obj_t* lbl, const char* text) {
  if (!lbl || !text) {
    return;
  }
  const char* prev = lv_label_get_text(lbl);
  if (prev && strcmp(prev, text) == 0) {
    return;
  }
  lv_label_set_text(lbl, text);
}

lv_obj_t* makePanel(lv_obj_t* parent, int x, int y, int w, int h) {
  lv_obj_t* obj = lv_obj_create(parent);
  lv_obj_set_pos(obj, x, y);
  lv_obj_set_size(obj, w, h);
  lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_bg_color(obj, lv_color_hex(kColPanel),
                            LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_border_color(obj, lv_color_hex(kColBorder),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_radius(obj, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
  return obj;
}

lv_obj_t* makeLabel(lv_obj_t* parent, int x, int y, int maxW, const char* text,
                    uint32_t color) {
  lv_obj_t* obj = lv_label_create(parent);
  lv_obj_set_pos(obj, x, y);
  if (maxW > 0) {
    lv_obj_set_width(obj, maxW);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_CLIP);
  }
  lv_obj_set_style_text_font(obj, kFont, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_color(obj, lv_color_hex(color),
                              LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_label_set_text(obj, text);
  return obj;
}

lv_obj_t* makeButton(lv_obj_t* parent, int x, int y, int w, int h,
                     const char* text, lv_event_cb_t cb, uint32_t bg,
                     uint32_t fg = 0xFFFFFFu) {
  lv_obj_t* obj = lv_button_create(parent);
  lv_obj_set_pos(obj, x, y);
  lv_obj_set_size(obj, w, h);
  lv_obj_set_style_bg_color(obj, lv_color_hex(bg), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_radius(obj, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_add_flag(obj, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE |
                                                   LV_OBJ_FLAG_PRESS_LOCK));
  lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(obj, cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* lbl = lv_label_create(obj);
  lv_obj_set_style_text_font(lbl, kFont, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_color(lbl, lv_color_hex(fg), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_label_set_text(lbl, text);
  lv_obj_center(lbl);
  lv_obj_add_flag(lbl, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
  return obj;
}

const char* runModeName(uint16_t mode) {
  switch (mode) {
    case 0:
      return "Off";
    case 1:
      return "Cool";
    case 2:
      return "Heat";
    case 3:
      return "DHW";
    case 4:
      return "Cool+DHW";
    case 5:
      return "Heat+DHW";
    case 7:
      return "Defrost";
    default:
      return "?";
  }
}

uint32_t s_busyUntilMs = 0;

WindmiHpConfigSnap readCfg() {
  WindmiHpConfigSnap cfg = {};
  lgModelReadHpConfigSnap(&cfg);
  return cfg;
}

void queueWrite(uint16_t addr, uint16_t value) {
  if (!rotensoBusQueueConfigWrite(addr, value)) {
    setLabelIfChanged(hpConfigObj.lbl_cfg_status, "Fronta zápisu plná");
    return;
  }
  setLabelIfChanged(hpConfigObj.lbl_cfg_status, "Zapisuji...");
  // Delší hold — poll až po WR + ~1.2 s (bus), ne hned (revert staré hodnoty).
  s_busyUntilMs = millis() + 5000u;
}

/** Optimistic patch snap (hned po +/-), dokud nepřijde poll. */
void patchCfg(void (*apply)(WindmiHpConfigSnap*)) {
  WindmiHpConfigSnap c = readCfg();
  apply(&c);
  c.valid = true;
  lgModelSetMbHpConfig(&c);
}

void onBack(lv_event_t* e) {
  (void)e;
  appCmdEnqueueHmi(UI_AKCE_PLAN_BACK);
}

void onPreset(lv_event_t* e) {
  (void)e;
  // Bez TUV + regulace na vodu + pevný SP + vnitřní IBH
  queueWrite((uint16_t)WINDMI_REG_CTRL_MODE, (uint16_t)WINDMI_CTRL_WATER);
  queueWrite((uint16_t)WINDMI_REG_CURVE_TYPE,
            (uint16_t)(int16_t)WINDMI_CURVE_FIXED_SP);
  queueWrite((uint16_t)WINDMI_REG_BACKUP_HEATER,
            (uint16_t)WINDMI_BACKUP_INNER_EH);
  setLabelIfChanged(hpConfigObj.lbl_cfg_status,
                    "Preset: voda / pevný SP / IBH=6");
}

void onRefresh(lv_event_t* e) {
  (void)e;
  rotensoBusRequestConfigPoll();
  setLabelIfChanged(hpConfigObj.lbl_cfg_status, "Obnovuji registry...");
}

void adjustX10(uint16_t addr, int16_t current, int delta, int16_t lo,
               int16_t hi) {
  int32_t next = (int32_t)current + (int32_t)delta;
  if (next < lo) {
    next = lo;
  }
  if (next > hi) {
    next = hi;
  }
  queueWrite(addr, (uint16_t)(int16_t)next);
}

void adjustU16(uint16_t addr, uint16_t current, int delta, uint16_t lo,
               uint16_t hi) {
  int32_t next = (int32_t)current + (int32_t)delta;
  if (next < (int32_t)lo) {
    next = lo;
  }
  if (next > (int32_t)hi) {
    next = hi;
  }
  queueWrite(addr, (uint16_t)next);
}

/** Manuál: jen 1=kontakty, 2=WUI. 0 TČ odmítá a vrací 1. */
uint16_t clampUiType(uint16_t v) {
  if (v >= (uint16_t)WINDMI_UI_WIRED) {
    return (uint16_t)WINDMI_UI_WIRED;
  }
  return (uint16_t)WINDMI_UI_CONTACTS;
}

void onUiTypeM(lv_event_t* e) {
  (void)e;
  const WindmiHpConfigSnap c = readCfg();
  if (!(c.mask & 0x0100u)) {
    rotensoBusRequestConfigPoll();
    setLabelIfChanged(hpConfigObj.lbl_cfg_status, "Nejdriv nacist (Obnovit)");
    return;
  }
  // 2 → 1 → 2 (bez 0)
  const uint16_t cur = clampUiType(c.ui_type);
  const uint16_t next = (cur == (uint16_t)WINDMI_UI_WIRED)
                            ? (uint16_t)WINDMI_UI_CONTACTS
                            : (uint16_t)WINDMI_UI_WIRED;
  queueWrite((uint16_t)WINDMI_REG_UI_TYPE, next);
  patchCfg([](WindmiHpConfigSnap* s) {
    const uint16_t cur = clampUiType(s->ui_type);
    s->ui_type = (cur == (uint16_t)WINDMI_UI_WIRED)
                     ? (uint16_t)WINDMI_UI_CONTACTS
                     : (uint16_t)WINDMI_UI_WIRED;
    s->mask |= 0x0100u;
  });
}
void onUiTypeP(lv_event_t* e) {
  (void)e;
  const WindmiHpConfigSnap c = readCfg();
  if (!(c.mask & 0x0100u)) {
    rotensoBusRequestConfigPoll();
    setLabelIfChanged(hpConfigObj.lbl_cfg_status, "Nejdriv nacist (Obnovit)");
    return;
  }
  // 1 → 2 → 1
  const uint16_t cur = clampUiType(c.ui_type);
  const uint16_t next = (cur == (uint16_t)WINDMI_UI_CONTACTS)
                            ? (uint16_t)WINDMI_UI_WIRED
                            : (uint16_t)WINDMI_UI_CONTACTS;
  queueWrite((uint16_t)WINDMI_REG_UI_TYPE, next);
  patchCfg([](WindmiHpConfigSnap* s) {
    const uint16_t cur = clampUiType(s->ui_type);
    s->ui_type = (cur == (uint16_t)WINDMI_UI_CONTACTS)
                     ? (uint16_t)WINDMI_UI_WIRED
                     : (uint16_t)WINDMI_UI_CONTACTS;
    s->mask |= 0x0100u;
  });
}

void onMinOatM(lv_event_t* e) {
  (void)e;
  const WindmiHpConfigSnap c = readCfg();
  if (!(c.mask & 0x08u)) {
    rotensoBusRequestConfigPoll();
    setLabelIfChanged(hpConfigObj.lbl_cfg_status, "Nejdřív načíst (Obnovit)");
    return;
  }
  adjustX10((uint16_t)WINDMI_REG_MIN_OAT_HEAT, c.min_oat_heat_x10, -10, -260,
            100);
  patchCfg([](WindmiHpConfigSnap* s) {
    int32_t n = (int32_t)s->min_oat_heat_x10 - 10;
    if (n < -260) n = -260;
    s->min_oat_heat_x10 = (int16_t)n;
    s->mask |= 0x08u;
  });
}
void onMinOatP(lv_event_t* e) {
  (void)e;
  const WindmiHpConfigSnap c = readCfg();
  if (!(c.mask & 0x08u)) {
    rotensoBusRequestConfigPoll();
    setLabelIfChanged(hpConfigObj.lbl_cfg_status, "Nejdřív načíst (Obnovit)");
    return;
  }
  adjustX10((uint16_t)WINDMI_REG_MIN_OAT_HEAT, c.min_oat_heat_x10, 10, -260,
            100);
  patchCfg([](WindmiHpConfigSnap* s) {
    int32_t n = (int32_t)s->min_oat_heat_x10 + 10;
    if (n > 100) n = 100;
    s->min_oat_heat_x10 = (int16_t)n;
    s->mask |= 0x08u;
  });
}
bool requireMask(uint16_t bit) {
  const WindmiHpConfigSnap c = readCfg();
  if ((c.mask & bit) == 0u) {
    rotensoBusRequestConfigPoll();
    setLabelIfChanged(hpConfigObj.lbl_cfg_status, "Nejdřív načíst (Obnovit)");
    return false;
  }
  return true;
}

void onIbhOatM(lv_event_t* e) {
  (void)e;
  if (!requireMask(0x40u)) return;
  const WindmiHpConfigSnap c = readCfg();
  adjustX10((uint16_t)WINDMI_REG_IBH_OAT, c.ibh_oat_x10, -10, -200, 150);
  patchCfg([](WindmiHpConfigSnap* s) {
    int32_t n = (int32_t)s->ibh_oat_x10 - 10;
    if (n < -200) n = -200;
    s->ibh_oat_x10 = (int16_t)n;
    s->mask |= 0x40u;
  });
}
void onIbhOatP(lv_event_t* e) {
  (void)e;
  if (!requireMask(0x40u)) return;
  const WindmiHpConfigSnap c = readCfg();
  adjustX10((uint16_t)WINDMI_REG_IBH_OAT, c.ibh_oat_x10, 10, -200, 150);
  patchCfg([](WindmiHpConfigSnap* s) {
    int32_t n = (int32_t)s->ibh_oat_x10 + 10;
    if (n > 150) n = 150;
    s->ibh_oat_x10 = (int16_t)n;
    s->mask |= 0x40u;
  });
}
void onIbhWarmM(lv_event_t* e) {
  (void)e;
  if (!requireMask(0x10u)) return;
  const WindmiHpConfigSnap c = readCfg();
  adjustU16((uint16_t)WINDMI_REG_IBH_WARMUP, c.ibh_warmup_min, -1, 0, 60);
  patchCfg([](WindmiHpConfigSnap* s) {
    if (s->ibh_warmup_min > 0) --s->ibh_warmup_min;
    s->mask |= 0x10u;
  });
}
void onIbhWarmP(lv_event_t* e) {
  (void)e;
  if (!requireMask(0x10u)) return;
  const WindmiHpConfigSnap c = readCfg();
  adjustU16((uint16_t)WINDMI_REG_IBH_WARMUP, c.ibh_warmup_min, 1, 0, 60);
  patchCfg([](WindmiHpConfigSnap* s) {
    if (s->ibh_warmup_min < 60) ++s->ibh_warmup_min;
    s->mask |= 0x10u;
  });
}
void onIbhDtM(lv_event_t* e) {
  (void)e;
  if (!requireMask(0x20u)) return;
  const WindmiHpConfigSnap c = readCfg();
  adjustX10((uint16_t)WINDMI_REG_IBH_DELTA_T, c.ibh_delta_t_x10, -5, 0, 200);
  patchCfg([](WindmiHpConfigSnap* s) {
    int32_t n = (int32_t)s->ibh_delta_t_x10 - 5;
    if (n < 0) n = 0;
    s->ibh_delta_t_x10 = (int16_t)n;
    s->mask |= 0x20u;
  });
}
void onIbhDtP(lv_event_t* e) {
  (void)e;
  if (!requireMask(0x20u)) return;
  const WindmiHpConfigSnap c = readCfg();
  adjustX10((uint16_t)WINDMI_REG_IBH_DELTA_T, c.ibh_delta_t_x10, 5, 0, 200);
  patchCfg([](WindmiHpConfigSnap* s) {
    int32_t n = (int32_t)s->ibh_delta_t_x10 + 5;
    if (n > 200) n = 200;
    s->ibh_delta_t_x10 = (int16_t)n;
    s->mask |= 0x20u;
  });
}
void onPumpDtM(lv_event_t* e) {
  (void)e;
  if (!requireMask(0x80u)) return;
  const WindmiHpConfigSnap c = readCfg();
  adjustX10((uint16_t)WINDMI_REG_PUMP_DELTA_T, c.pump_delta_t_x10, -5, 0, 150);
  patchCfg([](WindmiHpConfigSnap* s) {
    int32_t n = (int32_t)s->pump_delta_t_x10 - 5;
    if (n < 0) n = 0;
    s->pump_delta_t_x10 = (int16_t)n;
    s->mask |= 0x80u;
  });
}
void onPumpDtP(lv_event_t* e) {
  (void)e;
  if (!requireMask(0x80u)) return;
  const WindmiHpConfigSnap c = readCfg();
  adjustX10((uint16_t)WINDMI_REG_PUMP_DELTA_T, c.pump_delta_t_x10, 5, 0, 150);
  patchCfg([](WindmiHpConfigSnap* s) {
    int32_t n = (int32_t)s->pump_delta_t_x10 + 5;
    if (n > 150) n = 150;
    s->pump_delta_t_x10 = (int16_t)n;
    s->mask |= 0x80u;
  });
}

void makeParamRow(lv_obj_t* panel, int y, int panelW, const char* title,
                  lv_obj_t** lblVal, lv_obj_t** btnM, lv_obj_t** btnP,
                  lv_event_cb_t cbM, lv_event_cb_t cbP) {
  makeLabel(panel, kPad, y + 10, 280, title, kColMuted);
  *lblVal = makeLabel(panel, 280, y + 10, 160, "-", kColText);
  *btnM = makeButton(panel, panelW - kPad - 2 * kSmallBtnW - kGap, y + 2,
                     kSmallBtnW, kRowH - 4, "-", cbM, 0x48484Fu);
  *btnP = makeButton(panel, panelW - kPad - kSmallBtnW, y + 2, kSmallBtnW,
                     kRowH - 4, "+", cbP, 0x48484Fu);
}

void fmtX10(char* buf, size_t n, int16_t v, bool ok, const char* unit) {
  if (!ok) {
    snprintf(buf, n, "-");
    return;
  }
  snprintf(buf, n, "%.1f %s", (double)v * 0.1, unit);
}

}  // namespace

void uiHpConfigCreate(void) {
  if (s_created) {
    return;
  }
  memset(&hpConfigObj, 0, sizeof(hpConfigObj));

  lv_obj_t* scr = lv_obj_create(nullptr);
  hpConfigObj.screen = scr;
  lv_obj_set_size(scr, kW, kH);
  lv_obj_set_style_bg_color(scr, lv_color_hex(kColBg), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  hpConfigObj.btn_back =
      makeButton(scr, kMargin, 4, 120, kBtnH, "< ZPĚT", onBack, 0x48484Fu);

  hpConfigObj.lbl_title = makeLabel(scr, 0, 10, 0, "KONFIGURACE TČ", kColText);
  lv_obj_set_style_text_font(hpConfigObj.lbl_title, kFontTitle,
                             LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_align(hpConfigObj.lbl_title, LV_ALIGN_TOP_MID,
                         LV_PART_MAIN | LV_STATE_DEFAULT);

  hpConfigObj.btn_refresh =
      makeButton(scr, kW - kMargin - 150, 4, 140, kBtnH, "Obnovit", onRefresh,
                 kColAccent);
  hpConfigObj.btn_preset =
      makeButton(scr, kW - kMargin - 360, 4, 190, kBtnH, "Bez TUV+IBH", onPreset,
                 kColPurple);

  const int contentW = kW - 2 * kMargin;
  const int topY = 56;
  const int colW = (contentW - kGap) / 2;
  const int bodyH = kH - topY - kMargin;

  lv_obj_t* left = makePanel(scr, kMargin, topY, colW, bodyH);
  makeLabel(left, kPad, 6, colW - 2 * kPad, "Parametry (RW)", kColOrange);

  hpConfigObj.lbl_ctrl =
      makeLabel(left, kPad, 40, colW - 2 * kPad, "Regulace: -", kColText);
  hpConfigObj.lbl_curve =
      makeLabel(left, kPad, 70, colW - 2 * kPad, "Křivka: -", kColText);
  hpConfigObj.lbl_backup =
      makeLabel(left, kPad, 100, colW - 2 * kPad, "Záloha: -", kColText);

  int y = 130;
  makeParamRow(left, y, colW, "Drát. ovladač", &hpConfigObj.lbl_ui_type,
               &hpConfigObj.btn_ui_type_m, &hpConfigObj.btn_ui_type_p, onUiTypeM,
               onUiTypeP);
  y += kRowH + 4;
  makeParamRow(left, y, colW, "Min. OAT topení", &hpConfigObj.lbl_min_oat,
               &hpConfigObj.btn_min_oat_m, &hpConfigObj.btn_min_oat_p, onMinOatM,
               onMinOatP);
  y += kRowH + 4;
  makeParamRow(left, y, colW, "IBH práh OAT", &hpConfigObj.lbl_ibh_oat,
               &hpConfigObj.btn_ibh_oat_m, &hpConfigObj.btn_ibh_oat_p, onIbhOatM,
               onIbhOatP);
  y += kRowH + 4;
  makeParamRow(left, y, colW, "IBH warmup", &hpConfigObj.lbl_ibh_warm,
               &hpConfigObj.btn_ibh_warm_m, &hpConfigObj.btn_ibh_warm_p,
               onIbhWarmM, onIbhWarmP);
  y += kRowH + 4;
  makeParamRow(left, y, colW, "IBH dT", &hpConfigObj.lbl_ibh_dt,
               &hpConfigObj.btn_ibh_dt_m, &hpConfigObj.btn_ibh_dt_p, onIbhDtM,
               onIbhDtP);
  y += kRowH + 4;
  makeParamRow(left, y, colW, "Pump dT", &hpConfigObj.lbl_pump_dt,
               &hpConfigObj.btn_pump_dt_m, &hpConfigObj.btn_pump_dt_p, onPumpDtM,
               onPumpDtP);

  hpConfigObj.lbl_cfg_status =
      makeLabel(left, kPad, bodyH - 36, colW - 2 * kPad, "Čekám na Modbus...",
                kColMuted);

  lv_obj_t* right = makePanel(scr, kMargin + colW + kGap, topY, colW, bodyH);
  makeLabel(right, kPad, 6, colW - 2 * kPad, "Live diagnostika (RO)", kColOrange);
  hpConfigObj.lbl_live =
      makeLabel(right, kPad, 40, colW - 2 * kPad, "...", kColText);
  lv_obj_set_height(hpConfigObj.lbl_live, bodyH - 56);
  lv_label_set_long_mode(hpConfigObj.lbl_live, LV_LABEL_LONG_WRAP);

  s_created = true;
}

void uiHpConfigEnsureCreated(void) {
  if (!s_created) {
    uiHpConfigCreate();
  }
  if (!s_active) {
    s_active = true;
    rotensoBusSetConfigScreenActive(true);
    rotensoBusRequestConfigPoll();
  }
}

void uiHpConfigOnLeave(void) {
  if (s_active) {
    s_active = false;
    rotensoBusSetConfigScreenActive(false);
  }
}

void uiHpConfigTick(void) {
  if (!hpConfigObj.screen || !s_active) {
    return;
  }

  WindmiHpConfigSnap cfg = {};
  lgModelReadHpConfigSnap(&cfg);

  char line[96];
  if (cfg.mask & 0x01) {
    snprintf(line, sizeof(line), "Regulace: %s (100D=%u)",
             cfg.ctrl_mode == WINDMI_CTRL_WATER ? "voda" : "okolí",
             (unsigned)cfg.ctrl_mode);
  } else {
    snprintf(line, sizeof(line), "Regulace: -");
  }
  setLabelIfChanged(hpConfigObj.lbl_ctrl, line);

  if (cfg.mask & 0x02) {
    if (cfg.curve_type == (int16_t)WINDMI_CURVE_FIXED_SP) {
      snprintf(line, sizeof(line), "Křivka: pevný SP (0245=%d)",
               (int)cfg.curve_type);
    } else {
      snprintf(line, sizeof(line), "Křivka: typ %d", (int)cfg.curve_type);
    }
  } else {
    snprintf(line, sizeof(line), "Křivka: -");
  }
  setLabelIfChanged(hpConfigObj.lbl_curve, line);

  if (cfg.mask & 0x04) {
    snprintf(line, sizeof(line), "Záloha: %s (0259=%u)",
             cfg.backup_heater == WINDMI_BACKUP_INNER_EH ? "vnitřní EH"
                                                         : "jiné",
             (unsigned)cfg.backup_heater);
  } else {
    snprintf(line, sizeof(line), "Záloha: -");
  }
  setLabelIfChanged(hpConfigObj.lbl_backup, line);

  if (cfg.mask & 0x0100u) {
    const char* name = "?";
    if (cfg.ui_type == WINDMI_UI_CONTACTS) {
      name = "kontakty";  // bez dratoveho WUI (Modbus OK)
    } else if (cfg.ui_type == WINDMI_UI_WIRED) {
      name = "WUI";
    } else if (cfg.ui_type == WINDMI_UI_NONE) {
      name = "neplatne->1";
    }
    snprintf(line, sizeof(line), "%s (%u)", name, (unsigned)cfg.ui_type);
  } else {
    snprintf(line, sizeof(line), "-");
  }
  setLabelIfChanged(hpConfigObj.lbl_ui_type, line);

  fmtX10(line, sizeof(line), cfg.min_oat_heat_x10, (cfg.mask & 0x08) != 0, "°C");
  setLabelIfChanged(hpConfigObj.lbl_min_oat, line);
  fmtX10(line, sizeof(line), cfg.ibh_oat_x10, (cfg.mask & 0x40) != 0, "°C");
  setLabelIfChanged(hpConfigObj.lbl_ibh_oat, line);
  if (cfg.mask & 0x10) {
    snprintf(line, sizeof(line), "%u min", (unsigned)cfg.ibh_warmup_min);
  } else {
    snprintf(line, sizeof(line), "-");
  }
  setLabelIfChanged(hpConfigObj.lbl_ibh_warm, line);
  fmtX10(line, sizeof(line), cfg.ibh_delta_t_x10, (cfg.mask & 0x20) != 0, "°C");
  setLabelIfChanged(hpConfigObj.lbl_ibh_dt, line);
  fmtX10(line, sizeof(line), cfg.pump_delta_t_x10, (cfg.mask & 0x80) != 0, "°C");
  setLabelIfChanged(hpConfigObj.lbl_pump_dt, line);

  {
    const bool busy = (int32_t)(millis() - s_busyUntilMs) < 0;
    const char* prev = lv_label_get_text(hpConfigObj.lbl_cfg_status);
    const bool keepBusy =
        busy || (prev && (strncmp(prev, "Fronta", 6) == 0 ||
                          strncmp(prev, "Nejdřív", 7) == 0));
    if (!keepBusy) {
      if (cfg.valid) {
        setLabelIfChanged(hpConfigObj.lbl_cfg_status, "Config OK");
      } else if (!rotensoBusIsReady() || rotensoBusOkCount() == 0) {
        setLabelIfChanged(hpConfigObj.lbl_cfg_status, "Čekám na Modbus...");
      } else {
        setLabelIfChanged(hpConfigObj.lbl_cfg_status, "Načítám registry...");
      }
    }
  }

  WindmiLiveSnap live = {};
  lgModelReadLiveSnap(&live);

  char liveBuf[420];
  const float compHz =
      live.valid ? ((float)live.comp_freq_x10 *
                    (live.comp_freq_x10 > 200 ? 0.1f : 1.0f))
                 : 0.f;
  const float reqHz =
      live.req_comp_ok ? ((float)live.req_comp_freq_x10 *
                          (live.req_comp_freq_x10 > 200 ? 0.1f : 1.0f))
                       : 0.f;
  const float flow =
      live.valid ? ((float)live.water_flow_x100 * 0.01f) : 0.f;
  const bool ibh =
      live.load_ok && ((live.load_output & WINDMI_LOAD_IBH1_BIT) != 0u);

  lgModelLock();
  const float tin = mVstupniC;
  const float tout = mVystupniC;
  const float oat = mVenkovniC;
  const bool tinOk = mVstupniOk;
  const bool toutOk = mVystupniOk;
  const bool oatOk = mVenkovniOk;
  lgModelUnlock();

  WindmiAlarmSnap al{};
  lgModelReadAlarmSnap(&al);
  char alarmLine[64];
  if (al.valid) {
    snprintf(alarmLine, sizeof(alarmLine), "Alarm: %04X %04X %04X %04X%s",
             (unsigned)al.bm[0], (unsigned)al.bm[1], (unsigned)al.bm[2],
             (unsigned)al.bm[3],
             ((al.bm[0] & (1u << 9)) != 0u) ? " E9!" : "");
  } else {
    snprintf(alarmLine, sizeof(alarmLine), "Alarm: (nečteno)");
  }

  snprintf(
      liveBuf, sizeof(liveBuf),
      "Režim: %s (%u)\n"
      "Kompresor: %s%.0f Hz\n"
      "Požadavek: %s%.0f Hz\n"
      "Čerpadlo: %s%u\n"
      "Průtok: %s%.2f m³/h\n"
      "IBH1: %s\n"
      "Quiet: %s\n"
      "%s\n"
      "\n"
      "Venku: %s%.1f °C\n"
      "Tin: %s%.1f °C\n"
      "Tout: %s%.1f °C\n"
      "\n"
      "MB: %s (ok %lu)",
      live.valid ? runModeName(live.running_mode) : "-",
      live.valid ? (unsigned)live.running_mode : 0u,
      live.valid ? "" : "- ", live.valid ? (double)compHz : 0.0,
      live.req_comp_ok ? "" : "- ", live.req_comp_ok ? (double)reqHz : 0.0,
      live.valid ? "" : "- ", live.valid ? (unsigned)live.pump_speed : 0u,
      live.valid ? "" : "- ", live.valid ? (double)flow : 0.0,
      live.load_ok ? (ibh ? "ON" : "OFF") : "-",
      live.valid ? (live.quiet_night ? "ano" : "ne") : "-",
      alarmLine,
      oatOk ? "" : "- ", oatOk ? (double)oat : 0.0,
      tinOk ? "" : "- ", tinOk ? (double)tin : 0.0,
      toutOk ? "" : "- ", toutOk ? (double)tout : 0.0,
      rotensoBusIsReady() ? "live" : "-", rotensoBusOkCount());
  setLabelIfChanged(hpConfigObj.lbl_live, liveBuf);
}

lv_obj_t* uiHpConfigScreen(void) {
  return hpConfigObj.screen;
}
