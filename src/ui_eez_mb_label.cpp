#include "ui_eez_mb_label.h"

#include "bus_lg_model.h"
#include "ui_eez_fonts.h"
#include "ui_eez_nav.h"
#include "ui_eez_screens.h"
#include "ui_eez_status_bar.h"

#include <string.h>

namespace {

lv_obj_t* s_lblMb = nullptr;
char s_lastText[16] = "";

constexpr uint32_t kColOk = 0x30D158u;
constexpr uint32_t kColOff = 0x8E8E93u;

const char* mbLabelText() {
  return lgMaCerstoA0() ? "MB: OK" : "MB: ---";
}

uint32_t mbLabelColor() {
  return lgMaCerstoA0() ? kColOk : kColOff;
}

}  // namespace

void uiEezMbLabelInit(void) {
  if (!objects.main || s_lblMb) {
    return;
  }

  s_lblMb = lv_label_create(objects.main);
  lv_obj_set_pos(s_lblMb, UI_STATUS_MB_X, UI_STATUS_Y);
  lv_obj_set_style_text_font(s_lblMb, &ui_font_font_cs_24,
                             LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_color(s_lblMb, lv_color_hex(kColOff),
                              LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_align(s_lblMb, LV_TEXT_ALIGN_LEFT,
                              LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_remove_flag(s_lblMb, LV_OBJ_FLAG_CLICKABLE);
  lv_label_set_text(s_lblMb, "MB: ---");
  strncpy(s_lastText, "MB: ---", sizeof(s_lastText));
  s_lastText[sizeof(s_lastText) - 1] = '\0';
}

void uiEezMbLabelTick(void) {
  if (!s_lblMb || !uiIsMainScreen()) {
    return;
  }

  const char* text = mbLabelText();
  const uint32_t color = mbLabelColor();

  if (strcmp(text, s_lastText) != 0) {
    lv_label_set_text(s_lblMb, text);
    strncpy(s_lastText, text, sizeof(s_lastText));
    s_lastText[sizeof(s_lastText) - 1] = '\0';
  }

  static uint32_t s_lastColor = 0xFFFFFFFFu;
  if (s_lastColor != color) {
    s_lastColor = color;
    lv_obj_set_style_text_color(s_lblMb, lv_color_hex(color),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
  }
}
