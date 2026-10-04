#include "ui_eez_porucha.h"

#include "bus_lg_model.h"
#include "ui_eez_fonts.h"
#include "ui_eez_model.h"
#include "ui_eez_nav.h"
#include "ui_eez_screens.h"

#include <stdio.h>
#include <string.h>

namespace {

lv_obj_t* s_panel = nullptr;
lv_obj_t* s_lblTitle = nullptr;
lv_obj_t* s_lblText = nullptr;
char s_lastText[80] = "";
bool s_lastVisible = false;

constexpr uint32_t kColTitle = 0xFF453Au;
constexpr uint32_t kColText = 0xFFD60Au;
constexpr uint32_t kColPanel = 0x1A1A1Fu;
constexpr uint32_t kColBorder = 0x24242Bu;

struct HpAlarmBit {
  uint8_t map;  // 0..3 = 1009H..100CH
  uint8_t bit;  // 0..15
  const char* code;
  const char* text;
};

/** Manuál Windmi §14 alarm bitmapy + §13.1 kódy. */
constexpr HpAlarmBit kHpAlarms[] = {
    // 1009H #1 — E0…Ec
    {0, 0, "E0", "Prutokovy spinac vody"},
    {0, 1, "E1", "Komunikace IDU-ODU"},
    {0, 2, "E2", "Cidlo LWT za EH (T1)"},
    {0, 3, "E3", "Cidlo T2 BPHE (rez.)"},
    {0, 4, "E4", "Cidlo T2B BPHE (rez.)"},
    {0, 5, "E5", "Porucha venkovni jednotky"},
    {0, 6, "E6", "Cidlo zasobniku TUV (T7)"},
    {0, 7, "E7", "Cidlo vstupni vody EWT"},
    {0, 8, "E8", "Cidlo vystupni vody LWT"},
    {0, 9, "E9", "Komunikace dratovy ovladac"},
    {0, 10, "EA", "Cidlo 2. zony (Tw-2)"},
    {0, 11, "Eb", "Cidlo LWT pomocneho zdroje"},
    {0, 12, "Ec", "Porucha cerpadla vody"},
    // 100AH #2
    {1, 1, "P1", "Velky rozdil EWT-LWT"},
    {1, 2, "P2", "Nedostatek prutoku vody"},
    {1, 3, "P3", "Neobvykly rozdil EWT-LWT"},
    {1, 6, "EH", "Ochrana zpetne vazby EH"},
    // 100BH #3
    {2, 0, "E4o", "Cidlo teploty skraplace"},
    {2, 1, "E8o", "Cidlo teploty vytlaku"},
    {2, 3, "Pb", "Vysoka teplota BPHE (chladivo)"},
    {2, 4, "H4", "Ochrana P6 3x / 30 min"},
    {2, 5, "AC", "Neobvykle napeti AC"},
    {2, 6, "E4a", "Cidlo venkovni teploty"},
    {2, 7, "P3o", "Nadproudova ochrana"},
    {2, 8, "P6", "Ochrana IPM (P6)"},
    {2, 9, "H6", "Vysoka teplota 3x / 100 min"},
    {2, 10, "H12", "IPM vysoka teplota 3x / 60 min"},
    {2, 11, "E10", "Chyba EEPROM"},
    {2, 12, "P1o", "Ochrana vysokeho tlaku"},
    {2, 13, "H5", "Nizky tlak 3x / 30 min"},
    {2, 14, "H9", "Ventilator DC 2x / 10 min"},
    {2, 15, "P5", "Vysoka teplota skraplace"},
    // 100CH #4
    {3, 0, "E2r", "Komunikace IDU-ODU (rez.)"},
    {3, 1, "P9", "Ventilator ODU"},
    {3, 2, "Pb2", "Vysoka teplota IPM"},
    {3, 3, "H7", "Snizeni vykonu IDU (rez.)"},
    {3, 4, "H10", "Nadproud 3x / 60 min"},
    {3, 5, "P4", "Vysoka teplota vytlaku"},
    {3, 6, "Ec2", "Cidlo chlazeni PCB"},
    {3, 7, "P2o", "Ochrana nizkeho tlaku"},
};

void setVisible(bool on) {
  if (!s_panel) {
    return;
  }
  if (on == s_lastVisible) {
    return;
  }
  s_lastVisible = on;
  if (on) {
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
  }
}

/** Naplní msg prvním aktivním alarmem; vrací počet aktivních bitů. */
int formatHpAlarms(char* msg, size_t msgCap) {
  if (!msg || msgCap < 8) {
    return 0;
  }
  msg[0] = '\0';
  WindmiAlarmSnap al{};
  lgModelReadAlarmSnap(&al);
  if (!al.valid) {
    return 0;
  }

  int count = 0;
  char firstCode[12] = "";
  char firstText[48] = "";

  for (const HpAlarmBit& e : kHpAlarms) {
    if (e.map >= 4) {
      continue;
    }
    if ((al.bm[e.map] & (uint16_t)(1u << e.bit)) == 0u) {
      continue;
    }
    ++count;
    if (firstCode[0] == '\0') {
      strncpy(firstCode, e.code, sizeof(firstCode) - 1);
      strncpy(firstText, e.text, sizeof(firstText) - 1);
    }
  }

  // Neznámé bity (nejsou v tabulce)
  for (uint8_t m = 0; m < 4; ++m) {
    for (uint8_t b = 0; b < 16; ++b) {
      if ((al.bm[m] & (uint16_t)(1u << b)) == 0u) {
        continue;
      }
      bool known = false;
      for (const HpAlarmBit& e : kHpAlarms) {
        if (e.map == m && e.bit == b) {
          known = true;
          break;
        }
      }
      if (!known) {
        ++count;
        if (firstCode[0] == '\0') {
          snprintf(firstCode, sizeof(firstCode), "A%u.%u", (unsigned)m,
                   (unsigned)b);
          strncpy(firstText, "Neznamy alarm TC", sizeof(firstText) - 1);
        }
      }
    }
  }

  if (count == 0 || firstCode[0] == '\0') {
    return 0;
  }

  if (count > 1) {
    snprintf(msg, msgCap, "TC %s: %s (+%d)", firstCode, firstText, count - 1);
  } else {
    snprintf(msg, msgCap, "TC %s: %s", firstCode, firstText);
  }
  return count;
}

}  // namespace

static uint32_t s_poruchaBootMs = 0;

void uiEezRefreshPorucha(void) {
  if (s_poruchaBootMs == 0) {
    s_poruchaBootMs = millis();
  }
  char msg[sizeof(uiEez.porucha_text)] = "";

  // 1) Chyby z tepelného čerpadla (Modbus alarm bitmapy)
  if (formatHpAlarms(msg, sizeof(msg)) > 0) {
    // HP alarm má prioritu
  } else if (!lgMaCerstoA0()) {
    if ((millis() - s_poruchaBootMs) >= 45000u) {
      strncpy(msg, "Ztrata spojeni s venkovni jednotkou (Modbus)", sizeof(msg));
    }
  } else if (!uiRezimRegulatorWritesWater(uiEez.rezim) && uiEez.sp_pending != 0 &&
             uiEezTeplotaVodySetColor() == UI_SP_COLOR_WARN) {
    strncpy(msg, "Teplota vody nebyla potvrzena venkovni jednotkou", sizeof(msg));
  }

  msg[sizeof(msg) - 1] = '\0';
  if (strcmp(msg, uiEez.porucha_text) != 0) {
    strncpy(uiEez.porucha_text, msg, sizeof(uiEez.porucha_text));
    uiEez.porucha_text[sizeof(uiEez.porucha_text) - 1] = '\0';
  }
  uiEez.sig_alarm = uiEez.porucha_text[0] != '\0';
}

void uiEezPoruchaInit(void) {
  if (!objects.main || s_panel) {
    return;
  }

  s_panel = lv_obj_create(objects.main);
  lv_obj_set_pos(s_panel, 40, 528);
  lv_obj_set_size(s_panel, 770, 72);
  lv_obj_set_style_pad_all(s_panel, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_bg_color(s_panel, lv_color_hex(kColPanel), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_bg_opa(s_panel, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_border_color(s_panel, lv_color_hex(kColBorder), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_border_width(s_panel, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_radius(s_panel, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);

  s_lblTitle = lv_label_create(s_panel);
  lv_obj_set_pos(s_lblTitle, 4, 0);
  lv_obj_set_style_text_font(s_lblTitle, &ui_font_font_cs_24, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_color(s_lblTitle, lv_color_hex(kColTitle), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_label_set_text_static(s_lblTitle, "Poruchové hlášení");

  s_lblText = lv_label_create(s_panel);
  lv_obj_set_pos(s_lblText, 4, 30);
  lv_obj_set_width(s_lblText, 740);
  lv_obj_set_style_text_font(s_lblText, &ui_font_font_cs_24, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_color(s_lblText, lv_color_hex(kColText), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_label_set_long_mode(s_lblText, LV_LABEL_LONG_WRAP);
  lv_label_set_text(s_lblText, "");
}

void uiEezPoruchaTick(void) {
  if (!s_panel || !uiIsMainScreen()) {
    return;
  }

  const bool show = uiEez.porucha_text[0] != '\0';
  setVisible(show);
  if (!show) {
    return;
  }

  if (strcmp(uiEez.porucha_text, s_lastText) != 0) {
    lv_label_set_text(s_lblText, uiEez.porucha_text);
    strncpy(s_lastText, uiEez.porucha_text, sizeof(s_lastText));
    s_lastText[sizeof(s_lastText) - 1] = '\0';
  }
}
