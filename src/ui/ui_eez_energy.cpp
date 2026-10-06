#include "ui_eez_energy.h"

#include "lg_board.h"
#include "climate_energy.h"
#include "climate_plan.h"
#include "app_cmd.h"
#include "src/ui_eez_actions.h"
#include "src/ui_eez_fonts.h"
#include "src/ui_eez_model.h"
#include "src/ui_eez_nav.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

energy_objects_t energyObj;

namespace {

constexpr uint32_t kColBg = 0x121214u;
constexpr uint32_t kColPanel = 0x1A1A1Fu;
constexpr uint32_t kColBorder = 0x24242Bu;
constexpr uint32_t kColText = 0xE0E0E6u;
constexpr uint32_t kColMuted = 0x8E8E93u;
constexpr uint32_t kColAccent = 0x0A84FFu;
constexpr uint32_t kColOrange = 0xFF9F0Au;
constexpr uint32_t kColGreen = 0x30D158u;
constexpr uint32_t kColRed = 0xFF453Au;
constexpr uint32_t kColPurple = 0x5856D6u;
constexpr uint32_t kColUtlum = 0xC47A12u;
constexpr uint32_t kColVyp = 0x8B3A3Au;
constexpr uint32_t kColVypBright = 0xFF453Au;

constexpr int kW = BOARD_PANEL_W;
constexpr int kH = BOARD_PANEL_H;
constexpr int kMargin = 10;
constexpr int kGap = 8;
constexpr int kBtnH = 44;
constexpr int kPad = 8;
constexpr int kYAxisW = 52;
constexpr int kHourH = 22;
constexpr int kUtlumStripH = 12;
constexpr int kChartPoints = 288;  // 5min za den
constexpr int kMinPerPoint = ENERGY_MINUTES_PER_DAY / kChartPoints;  // 5
constexpr int kYTicks = 5;

const lv_font_t* kFont = &ui_font_font_cs_24;
const lv_font_t* kFontSummary = &ui_font_font_cs_28;
const lv_font_t* kFontHour = &lv_font_montserrat_14;
bool s_created = false;
int s_dayOffset = 0;
uint32_t s_lastGen = 0;

lv_obj_t* s_lblHour[24] = {};
lv_obj_t* s_lblYTick[kYTicks] = {};
lv_obj_t* s_lblMonthVal[ENERGY_SEASON_MONTHS] = {};
lv_obj_t* s_lblMonthName[ENERGY_SEASON_MONTHS] = {};
lv_obj_t* s_utlumStrip = nullptr;
lv_obj_t* s_powerPanel = nullptr;
lv_obj_t* s_monthPanel = nullptr;
int s_monthNameY = 0;

const char* kMonthNames[ENERGY_SEASON_MONTHS] = {
    "Zář", "Říj", "Lis", "Pro", "Led", "Úno", "Bře", "Dub", "Kvě"};

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

void setTextColor(lv_obj_t* obj, uint32_t color) {
  if (!obj) {
    return;
  }
  lv_obj_set_style_text_color(obj, lv_color_hex(color), LV_PART_MAIN | LV_STATE_DEFAULT);
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
                     const char* text, lv_event_cb_t cb, uint32_t bg) {
  lv_obj_t* obj = lv_button_create(parent);
  lv_obj_set_pos(obj, x, y);
  lv_obj_set_size(obj, w, h);
  lv_obj_set_style_bg_color(obj, lv_color_hex(bg), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_radius(obj, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_add_flag(obj, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE |
                                                    LV_OBJ_FLAG_PRESS_LOCK));
  lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
  if (cb) {
    lv_obj_add_event_cb(obj, cb, LV_EVENT_CLICKED, nullptr);
  }
  lv_obj_t* lbl = lv_label_create(obj);
  lv_obj_set_style_text_font(lbl, kFont, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFFu),
                              LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_label_set_text(lbl, text);
  lv_obj_center(lbl);
  lv_obj_add_flag(lbl, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
  return obj;
}

void onBack(lv_event_t* e) {
  (void)e;
  appCmdEnqueueHmi(UI_AKCE_PLAN_BACK);
}

void onDayPrev(lv_event_t* e) {
  (void)e;
  if (s_dayOffset < ENERGY_WEEK_DAYS - 1) {
    s_dayOffset++;
    s_lastGen = 0;
  }
}

void onDayNext(lv_event_t* e) {
  (void)e;
  if (s_dayOffset > 0) {
    s_dayOffset--;
    s_lastGen = 0;
  }
}

int weekdayFromYmd(int ymd) {
  if (ymd <= 0) {
    time_t now = time(nullptr);
    struct tm t{};
    localtime_r(&now, &t);
    return (t.tm_wday + 6) % 7;
  }
  struct tm t{};
  t.tm_year = (ymd / 10000) - 1900;
  t.tm_mon = ((ymd / 100) % 100) - 1;
  t.tm_mday = ymd % 100;
  t.tm_hour = 12;
  time_t tt = mktime(&t);
  struct tm out{};
  localtime_r(&tt, &out);
  return (out.tm_wday + 6) % 7;
}

uint16_t planMinutes(const PlanCas* c) {
  return (uint16_t)(c->hodina * 60 + c->minuta);
}

/** X v rámci plotu — přesně podle hodin (stejně jako svislá mřížka 24 sloupců). */
int xInPlot(int minute, int plotW) {
  if (plotW <= 0) {
    return 0;
  }
  if (minute <= 0) {
    return 0;
  }
  if (minute >= ENERGY_MINUTES_PER_DAY) {
    return plotW;
  }
  return (int)((int64_t)minute * plotW / ENERGY_MINUTES_PER_DAY);
}

void layoutHourLabels(int plotX, int plotW, int hourY) {
  for (int h = 0; h < 24; ++h) {
    if (!s_lblHour[h]) {
      continue;
    }
    // Sloupec hodiny h: [h/24 … (h+1)/24] šířky — stejně jako mřížka
    const int x0 = plotX + (h * plotW) / 24;
    const int x1 = plotX + ((h + 1) * plotW) / 24;
    int w = x1 - x0;
    if (w < 8) {
      w = 8;
    }
    lv_obj_set_pos(s_lblHour[h], x0, hourY);
    lv_obj_set_width(s_lblHour[h], w);
  }
}

/** Akce plánu platná v dané minutě zobrazeného dne (vč. Noc přes půlnoc). */
PlanAkce planAkceAt(int weekday, int minuteOfDay) {
  const PlanTydenConfig* cfg = climatePlanGetConfig();
  if (!cfg || !cfg->aktivni || weekday < 0 || weekday >= PLAN_POCET_DNU) {
    return PLAN_AKCE_NORMAL;
  }
  if (minuteOfDay < 0) {
    minuteOfDay += ENERGY_MINUTES_PER_DAY;
  }
  minuteOfDay %= ENERGY_MINUTES_PER_DAY;

  PlanAkce found = PLAN_AKCE_NORMAL;
  for (int ob = 0; ob < PLAN_POCET_OBDOBI; ++ob) {
    const PlanObdobiCas* od = &cfg->obdobi[ob];
    const int a = (int)planMinutes(&od->zacatek);
    const int b = (int)planMinutes(&od->konec);
    int cellDay = weekday;
    bool inRange = false;
    if (a == b) {
      continue;
    }
    if (a < b) {
      inRange = (minuteOfDay >= a && minuteOfDay < b);
    } else if (minuteOfDay >= a) {
      inRange = true;
    } else if (minuteOfDay < b) {
      inRange = true;
      cellDay = (weekday + 6) % PLAN_POCET_DNU;
    }
    if (!inRange) {
      continue;
    }
    const PlanAkce akce = cfg->tabulka[cellDay][ob].akce;
    if (akce == PLAN_AKCE_VYP) {
      return PLAN_AKCE_VYP;
    }
    if (akce == PLAN_AKCE_UTLUM) {
      found = PLAN_AKCE_UTLUM;
    }
  }
  return found;
}

void addPlanBand(lv_obj_t* parent, int plotW, int layerH, int fromMin, int toMin,
                 PlanAkce akce, lv_opa_t opa) {
  if (!parent || toMin <= fromMin || akce == PLAN_AKCE_NORMAL) {
    return;
  }
  const int x = xInPlot(fromMin, plotW);
  int w = xInPlot(toMin, plotW) - x;
  if (w < 2) {
    w = 2;
  }
  const uint32_t col = (akce == PLAN_AKCE_VYP) ? kColVyp : kColUtlum;
  lv_obj_t* band = lv_obj_create(parent);
  lv_obj_remove_style_all(band);
  lv_obj_set_pos(band, x, 0);
  lv_obj_set_size(band, w, layerH);
  lv_obj_set_style_bg_color(band, lv_color_hex(col), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_bg_opa(band, opa, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_remove_flag(band, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(band, LV_OBJ_FLAG_CLICKABLE);
}

/** Spojí souvislé úseky stejné akce (po minutách) do sloupců. */
void paintPlanSpans(lv_obj_t* parent, int plotW, int layerH, int weekday,
                    lv_opa_t opaUtlum, lv_opa_t opaVyp) {
  if (!parent || plotW <= 0 || layerH <= 0) {
    return;
  }
  PlanAkce run = PLAN_AKCE_NORMAL;
  int runStart = 0;
  for (int m = 0; m <= ENERGY_MINUTES_PER_DAY; ++m) {
    const PlanAkce cur =
        (m < ENERGY_MINUTES_PER_DAY) ? planAkceAt(weekday, m) : PLAN_AKCE_NORMAL;
    if (cur != run) {
      if (run != PLAN_AKCE_NORMAL) {
        const lv_opa_t opa = (run == PLAN_AKCE_VYP) ? opaVyp : opaUtlum;
        addPlanBand(parent, plotW, layerH, runStart, m, run, opa);
      }
      run = cur;
      runStart = m;
    }
  }
}

void syncPlotGeometry(int* plotX, int* plotW, int* plotY, int* plotH) {
  if (!energyObj.chart_power) {
    if (plotX) *plotX = 0;
    if (plotW) *plotW = 0;
    if (plotY) *plotY = 0;
    if (plotH) *plotH = 0;
    return;
  }
  // Vynutit nulové okraje — jinak content ≠ objekt a overlay ujede o pár px
  lv_obj_set_style_pad_all(energyObj.chart_power, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_border_width(energyObj.chart_power, 0,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
  const int x = (int)lv_obj_get_x(energyObj.chart_power);
  const int y = (int)lv_obj_get_y(energyObj.chart_power);
  const int w = (int)lv_obj_get_content_width(energyObj.chart_power);
  const int h = (int)lv_obj_get_content_height(energyObj.chart_power);
  if (plotX) *plotX = x;
  if (plotY) *plotY = y;
  if (plotW) *plotW = w > 0 ? w : (int)lv_obj_get_width(energyObj.chart_power);
  if (plotH) *plotH = h > 0 ? h : (int)lv_obj_get_height(energyObj.chart_power);

  if (energyObj.band_layer) {
    lv_obj_set_pos(energyObj.band_layer, *plotX, *plotY);
    lv_obj_set_size(energyObj.band_layer, *plotW, *plotH);
  }
  if (s_utlumStrip) {
    const int stripY = *plotY + *plotH + 2 + kHourH - 2;
    lv_obj_set_pos(s_utlumStrip, *plotX, stripY);
    lv_obj_set_size(s_utlumStrip, *plotW, kUtlumStripH);
  }
}

void refreshUtlumAxis(int weekday) {
  int plotX = 0, plotW = 0, plotY = 0, plotH = 0;
  syncPlotGeometry(&plotX, &plotW, &plotY, &plotH);
  if (!s_utlumStrip || plotW <= 0) {
    return;
  }
  lv_obj_clean(s_utlumStrip);
  const int stripH = (int)lv_obj_get_height(s_utlumStrip);
  paintPlanSpans(s_utlumStrip, plotW, stripH, weekday, LV_OPA_COVER, LV_OPA_COVER);

  const int hourY = plotY + plotH + 2;
  layoutHourLabels(plotX, plotW, hourY);

  for (int h = 0; h < 24; ++h) {
    const PlanAkce akce = planAkceAt(weekday, h * 60 + 30);
    uint32_t col = kColMuted;
    if (akce == PLAN_AKCE_UTLUM) {
      col = kColOrange;
    } else if (akce == PLAN_AKCE_VYP) {
      col = kColVypBright;
    }
    setTextColor(s_lblHour[h], col);
  }
}

void refreshBands(int weekday) {
  int plotX = 0, plotW = 0, plotY = 0, plotH = 0;
  syncPlotGeometry(&plotX, &plotW, &plotY, &plotH);
  if (!energyObj.band_layer || plotW <= 0) {
    return;
  }
  lv_obj_clean(energyObj.band_layer);
  paintPlanSpans(energyObj.band_layer, plotW, plotH, weekday, LV_OPA_30, LV_OPA_40);
}

void refreshYTicks(int32_t maxTenthKw) {
  // maxTenthKw násobek 20 → 5 ticků po 0,5 kW (2.0 / 1.5 / 1.0 / 0.5 / 0)
  if (maxTenthKw < 20) {
    maxTenthKw = 20;
  }
  if (!energyObj.chart_power) {
    return;
  }

  lv_obj_t* chart = energyObj.chart_power;
  // Stejná geometrie jako draw_series_line / draw_div_lines v LVGL
  const int32_t border = lv_obj_get_style_border_width(chart, LV_PART_MAIN);
  const int32_t padTop =
      lv_obj_get_style_pad_top(chart, LV_PART_MAIN) + border;
  const int32_t h = lv_obj_get_content_height(chart);
  const int chartBaseY = (int)lv_obj_get_y(chart) + (int)padTop;
  const int labelH = (int)lv_font_get_line_height(kFontHour);

  for (int i = 0; i < kYTicks; ++i) {
    if (!s_lblYTick[i]) {
      continue;
    }
    // Hodnota ticku (desetiny kW) — integer, max násobek 4 ⇒ přesně na mřížce
    const int32_t tenth =
        maxTenthKw * (kYTicks - 1 - i) / (kYTicks - 1);
    char buf[16];
    if ((tenth % 10) == 0) {
      snprintf(buf, sizeof(buf), "%ld", (long)(tenth / 10));
    } else {
      snprintf(buf, sizeof(buf), "%ld.%ld", (long)(tenth / 10),
               (long)(tenth % 10));
    }
    setLabelIfChanged(s_lblYTick[i], buf);

    // Y jako u křivky: y = h - (val - ymin) * h / (ymax - ymin)
    int yLine = chartBaseY;
    if (maxTenthKw > 0 && h > 0) {
      const int32_t yTmp = (int32_t)(((int64_t)tenth * h) / maxTenthKw);
      yLine = chartBaseY + (int)(h - yTmp);
    }
    // Střed textu na čáře mřížky (= hodnota křivky)
    lv_obj_set_y(s_lblYTick[i], yLine - labelH / 2);
  }
}

/** Příkon pro graf: PZEM W + 3 kW při el. topení (jako souhrn). */
uint16_t sampleDisplayWatts(uint16_t raw) {
  uint32_t w = climateEnergySampleWatts(raw);
  if (climateEnergySampleAux(raw)) {
    w += 3000u;
  }
  if (w > ENERGY_PWR_W_MASK) {
    w = ENERGY_PWR_W_MASK;
  }
  return (uint16_t)w;
}

int32_t niceMaxTenthKw(uint32_t maxW) {
  // Desetiny kW → nahoru na násobek 2,0 kW (20), min. 2,0 kW
  // 5 ticků: 2.0 / 1.5 / 1.0 / 0.5 / 0 (ne 0.2, 0.7, …)
  int32_t maxTenth = (int32_t)((maxW + 99u) / 100u);
  maxTenth = ((maxTenth + 19) / 20) * 20;
  if (maxTenth < 20) {
    maxTenth = 20;
  }
  return maxTenth;
}

void refreshPowerChart() {
  const uint16_t* samples = nullptr;
  float dayKwh = 0.0f;
  int ymd = 0;
  if (!climateEnergyDayPowerGet(s_dayOffset, &samples, &dayKwh, &ymd) ||
      !samples || !energyObj.ser_power) {
    return;
  }

  const int step = kMinPerPoint;
  int32_t pointTenth[kChartPoints];
  bool pointAux[kChartPoints];
  uint32_t maxW = 0;

  for (int i = 0; i < kChartPoints; ++i) {
    uint32_t sum = 0;
    uint32_t n = 0;
    bool auxOn = false;
    for (int j = 0; j < step; ++j) {
      const uint16_t raw = samples[i * step + j];
      const uint16_t w = sampleDisplayWatts(raw);
      if (climateEnergySampleAux(raw)) {
        auxOn = true;
      }
      if (w > 0) {
        sum += w;
        ++n;
      }
    }
    pointAux[i] = auxOn;
    if (n == 0) {
      pointTenth[i] = LV_CHART_POINT_NONE;
    } else {
      const uint32_t avgW = sum / n;
      if (avgW > maxW) {
        maxW = avgW;
      }
      pointTenth[i] = (int32_t)((avgW + 50u) / 100u);  // 0,1 kW
    }
  }

  const int32_t maxTenth = niceMaxTenthKw(maxW);
  lv_chart_set_range(energyObj.chart_power, LV_CHART_AXIS_PRIMARY_Y, 0, maxTenth);
  lv_chart_set_range(energyObj.chart_power, LV_CHART_AXIS_SECONDARY_Y, 0, 1);
  refreshYTicks(maxTenth);

  for (int i = 0; i < kChartPoints; ++i) {
    lv_chart_set_value_by_id(energyObj.chart_power, energyObj.ser_power, i,
                             pointTenth[i]);
    if (energyObj.ser_aux) {
      lv_chart_set_value_by_id(energyObj.chart_power, energyObj.ser_aux, i,
                               pointAux[i] ? 1 : 0);
    }
  }
  lv_chart_refresh(energyObj.chart_power);

  char dayBuf[48];
  if (ymd > 0) {
    snprintf(dayBuf, sizeof(dayBuf), "%02d.%02d.%04d", ymd % 100,
             (ymd / 100) % 100, ymd / 10000);
  } else if (s_dayOffset == 0) {
    snprintf(dayBuf, sizeof(dayBuf), "Dnes");
  } else {
    snprintf(dayBuf, sizeof(dayBuf), "-%d d", s_dayOffset);
  }
  setLabelIfChanged(energyObj.lbl_day, dayBuf);

  char kwhBuf[48];
  snprintf(kwhBuf, sizeof(kwhBuf), "Den: %.2f kWh", (double)dayKwh);
  setLabelIfChanged(energyObj.lbl_day_kwh, kwhBuf);

  const int wd = weekdayFromYmd(ymd);
  refreshBands(wd);
  refreshUtlumAxis(wd);
  if (energyObj.band_layer && energyObj.chart_power) {
    lv_obj_move_background(energyObj.band_layer);
  }
}

/** Stejná X geometrie jako LVGL draw_series_bar (pad_column = mezera mezi sloupci). */
void monthBarSlot(int i, int contentW, int blockGap, int* xOut, int* wOut) {
  const int n = ENERGY_SEASON_MONTHS;
  int blockW = (contentW - (n - 1) * blockGap) / n;
  if (blockW < 1) {
    blockW = 1;
  }
  int x = 0;
  if (n > 1) {
    x = (int)((int64_t)(contentW - blockW) * i / (n - 1));
  }
  if (xOut) {
    *xOut = x;
  }
  if (wOut) {
    *wOut = blockW;
  }
}

/** Oranžová kWh + názvy měsíců — vždy stejné sloupce jako bary. */
void layoutMonthLabels(int valY) {
  if (!energyObj.chart_month) {
    return;
  }
  // Po layoutu panelu už má chart finální rozměr
  const int chartX = (int)lv_obj_get_x(energyObj.chart_month);
  const int chartY = (int)lv_obj_get_y(energyObj.chart_month);
  const int padL = (int)lv_obj_get_style_pad_left(energyObj.chart_month,
                                                   LV_PART_MAIN);
  const int border = (int)lv_obj_get_style_border_width(energyObj.chart_month,
                                                        LV_PART_MAIN);
  int contentW = (int)lv_obj_get_content_width(energyObj.chart_month);
  if (contentW <= 0) {
    contentW = (int)lv_obj_get_width(energyObj.chart_month);
  }
  const int blockGap = (int)lv_obj_get_style_pad_column(energyObj.chart_month,
                                                        LV_PART_MAIN);
  const int baseX = chartX + padL + border;
  const int yVal = (valY >= 0) ? valY : (chartY + 2);
  const int yName = s_monthNameY;

  for (int i = 0; i < ENERGY_SEASON_MONTHS; ++i) {
    int bx = 0, bw = 0;
    monthBarSlot(i, contentW, blockGap, &bx, &bw);
    if (s_lblMonthVal[i]) {
      lv_obj_set_pos(s_lblMonthVal[i], baseX + bx, yVal);
      lv_obj_set_width(s_lblMonthVal[i], bw);
    }
    if (s_lblMonthName[i]) {
      lv_obj_set_pos(s_lblMonthName[i], baseX + bx, yName);
      lv_obj_set_width(s_lblMonthName[i], bw);
    }
  }
}

void refreshMonthChart() {
  if (!energyObj.ser_month || !energyObj.chart_month) {
    return;
  }

  float vals[ENERGY_SEASON_MONTHS];
  float dataMax = 0.0f;
  for (int i = 0; i < ENERGY_SEASON_MONTHS; ++i) {
    vals[i] = climateEnergySeasonMonthKwh(i);
    if (vals[i] > dataMax) {
      dataMax = vals[i];
    }
  }

  // Dynamické měřítko — pevných 2000 kWh dělalo z 11 kWh neviditelný sloupec.
  int32_t yMax = 20;
  if (dataMax > 0.0f) {
    const float padded = dataMax * 1.25f;
    if (padded <= 20.0f) {
      yMax = 20;
    } else if (padded <= 50.0f) {
      yMax = 50;
    } else if (padded <= 100.0f) {
      yMax = 100;
    } else if (padded <= 200.0f) {
      yMax = 200;
    } else if (padded <= 500.0f) {
      yMax = 500;
    } else if (padded <= 1000.0f) {
      yMax = 1000;
    } else {
      yMax = ((int32_t)(padded + 99.0f) / 100) * 100;
    }
  }

  lv_chart_set_range(energyObj.chart_month, LV_CHART_AXIS_PRIMARY_Y, 0, yMax);
  for (int i = 0; i < ENERGY_SEASON_MONTHS; ++i) {
    int32_t v = (int32_t)(vals[i] + 0.5f);
    if (v < 0) {
      v = 0;
    }
    // Tiny non-zero still visible as at least 1 chart unit when scale is large
    if (vals[i] > 0.05f && v < 1) {
      v = 1;
    }
    if (v > yMax) {
      v = yMax;
    }
    lv_chart_set_value_by_id(energyObj.chart_month, energyObj.ser_month, i, v);
  }
  lv_chart_refresh(energyObj.chart_month);

  for (int i = 0; i < ENERGY_SEASON_MONTHS; ++i) {
    if (!s_lblMonthVal[i]) {
      continue;
    }
    char buf[16];
    if (vals[i] < 0.05f) {
      snprintf(buf, sizeof(buf), "0");
    } else if (vals[i] >= 100.0f) {
      snprintf(buf, sizeof(buf), "%.0f", (double)vals[i]);
    } else {
      snprintf(buf, sizeof(buf), "%.1f", (double)vals[i]);
    }
    setLabelIfChanged(s_lblMonthVal[i], buf);
  }

  const int chartY = (int)lv_obj_get_y(energyObj.chart_month);
  layoutMonthLabels(chartY + 2);
}

void refreshSummaryAndYears() {
  char buf[200];
  const char* monLbl = climateEnergySeasonMonthLabel();
  const float monKwh = climateEnergyMonthKwh();
  const int seasonY = climateEnergyCurrentSeasonYear();
  char seasonSpan[16];
  if (seasonY > 0) {
    snprintf(seasonSpan, sizeof(seasonSpan), "%d/%02d", seasonY,
             (seasonY + 1) % 100);
  } else {
    snprintf(seasonSpan, sizeof(seasonSpan), "-");
  }
  const float seasonKwh = climateEnergySeasonTotalKwh();
  // Součty z NVS; měsíc = slot v topné sezóně (ne kalendářní rok)
  if (climateEnergyIsOk() || climateEnergyAuxHeatOn()) {
    snprintf(buf, sizeof(buf),
             "Příkon %u W   Dnes %.2f kWh   %s %.1f kWh   Sezóna %s %.1f kWh",
             (unsigned)climateEnergyPowerW(),
             (double)climateEnergyTodayKwh(), monLbl, (double)monKwh,
             seasonSpan, (double)seasonKwh);
  } else {
    snprintf(buf, sizeof(buf),
             "Příkon - W   Dnes %.2f kWh   %s %.1f kWh   Sezóna %s %.1f kWh",
             (double)climateEnergyTodayKwh(), monLbl, (double)monKwh,
             seasonSpan, (double)seasonKwh);
  }
  setLabelIfChanged(energyObj.lbl_summary, buf);

  char ybuf[220];
  size_t n = 0;
  n += (size_t)snprintf(ybuf + n, sizeof(ybuf) - n, "Sezóny: ");
  bool any = false;
  for (int i = 0; i < ENERGY_YEAR_SLOTS; ++i) {
    int year = 0;
    float kwh = 0.0f;
    if (!climateEnergyYearGet(i, &year, &kwh)) {
      continue;
    }
    // Slot 0 = aktuální sezóna (živá); 1+ = uzavřené.
    any = true;
    n += (size_t)snprintf(ybuf + n, sizeof(ybuf) - n, "%d/%02d %.1f  ", year,
                          (year + 1) % 100, (double)kwh);
  }
  if (!any) {
    snprintf(ybuf, sizeof(ybuf), "Sezóny: -");
  }
  setLabelIfChanged(energyObj.lbl_years, ybuf);
}

}  // namespace

void uiEnergyCreate(void) {
  if (s_created) {
    return;
  }
  memset(&energyObj, 0, sizeof(energyObj));
  memset(s_lblHour, 0, sizeof(s_lblHour));
  memset(s_lblYTick, 0, sizeof(s_lblYTick));
  memset(s_lblMonthVal, 0, sizeof(s_lblMonthVal));
  memset(s_lblMonthName, 0, sizeof(s_lblMonthName));

  energyObj.screen = lv_obj_create(nullptr);
  lv_obj_set_size(energyObj.screen, kW, kH);
  lv_obj_set_style_bg_color(energyObj.screen, lv_color_hex(kColBg),
                            LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_bg_opa(energyObj.screen, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_remove_flag(energyObj.screen, LV_OBJ_FLAG_SCROLLABLE);

  energyObj.btn_back =
      makeButton(energyObj.screen, kMargin, kMargin, 100, kBtnH, "Zpět", onBack,
                 kColAccent);
  energyObj.lbl_title =
      makeLabel(energyObj.screen, kMargin + 110, kMargin + 8, 400, "Spotřeba TČ",
                kColText);
  energyObj.lbl_summary =
      makeLabel(energyObj.screen, kMargin, kMargin + kBtnH + 4, kW - 2 * kMargin,
                "---", kColMuted);
  lv_obj_set_style_text_font(energyObj.lbl_summary, kFontSummary,
                             LV_PART_MAIN | LV_STATE_DEFAULT);

  const int yearsH = 44;
  const int topY = kMargin + kBtnH + 4 + 36;
  const int bottomY = kH - kMargin - yearsH;
  const int stackH = bottomY - topY - kGap;
  const int powerH = stackH * 58 / 100;
  const int monthH = stackH - powerH - kGap;
  const int fullW = kW - 2 * kMargin;

  s_powerPanel = makePanel(energyObj.screen, kMargin, topY, fullW, powerH);
  energyObj.btn_day_prev =
      makeButton(s_powerPanel, kPad, kPad, 56, 36, "<", onDayPrev, kColPurple);
  energyObj.btn_day_next =
      makeButton(s_powerPanel, kPad + 64, kPad, 56, 36, ">", onDayNext, kColPurple);
  energyObj.lbl_day =
      makeLabel(s_powerPanel, kPad + 140, kPad + 6, 220, "Dnes", kColText);
  energyObj.lbl_day_kwh =
      makeLabel(s_powerPanel, fullW - 280, kPad + 6, 260, "Den: —", kColOrange);

  // kW nad osou Y — nesmí překrývat popisky stupnice
  const int chartY = kPad + 56;
  energyObj.lbl_y_unit =
      makeLabel(s_powerPanel, kPad, chartY - 20, kYAxisW - 4, "kW", kColMuted);
  lv_obj_set_style_text_font(energyObj.lbl_y_unit, kFontHour,
                             LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_align(energyObj.lbl_y_unit, LV_TEXT_ALIGN_RIGHT,
                              LV_PART_MAIN | LV_STATE_DEFAULT);

  const int chartInnerH = powerH - chartY - kPad - kHourH - kUtlumStripH - 4;
  const int chartInnerW = fullW - 2 * kPad - kYAxisW;
  const int chartX = kPad + kYAxisW;

  for (int i = 0; i < kYTicks; ++i) {
    s_lblYTick[i] =
        makeLabel(s_powerPanel, kPad, chartY, kYAxisW - 4, "0", kColMuted);
    lv_obj_set_style_text_font(s_lblYTick[i], kFontHour,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(s_lblYTick[i], LV_TEXT_ALIGN_RIGHT,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
  }

  energyObj.band_layer = lv_obj_create(s_powerPanel);
  lv_obj_remove_style_all(energyObj.band_layer);
  lv_obj_set_pos(energyObj.band_layer, chartX, chartY);
  lv_obj_set_size(energyObj.band_layer, chartInnerW, chartInnerH);
  lv_obj_set_style_bg_opa(energyObj.band_layer, LV_OPA_TRANSP,
                          LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_remove_flag(energyObj.band_layer, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(energyObj.band_layer, LV_OBJ_FLAG_CLICKABLE);

  energyObj.chart_power = lv_chart_create(s_powerPanel);
  lv_obj_set_pos(energyObj.chart_power, chartX, chartY);
  lv_obj_set_size(energyObj.chart_power, chartInnerW, chartInnerH);
  lv_obj_set_style_bg_opa(energyObj.chart_power, LV_OPA_TRANSP,
                          LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_pad_all(energyObj.chart_power, 0,
                           LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_border_width(energyObj.chart_power, 0,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_border_side(energyObj.chart_power, LV_BORDER_SIDE_NONE,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_border_opa(energyObj.chart_power, LV_OPA_TRANSP,
                              LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_radius(energyObj.chart_power, 0,
                          LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_remove_flag(energyObj.chart_power, LV_OBJ_FLAG_SCROLLABLE);
  lv_chart_set_type(energyObj.chart_power, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(energyObj.chart_power, kChartPoints);
  lv_chart_set_range(energyObj.chart_power, LV_CHART_AXIS_PRIMARY_Y, 0, 20);
  lv_chart_set_range(energyObj.chart_power, LV_CHART_AXIS_SECONDARY_Y, 0, 1);
  // hdiv = počet vodorovných čar (= kYTicks); musí sedět s popisky Y
  lv_chart_set_div_line_count(energyObj.chart_power, kYTicks, 25);
  lv_obj_set_style_line_width(energyObj.chart_power, 3, LV_PART_ITEMS);
  lv_obj_set_style_bg_opa(energyObj.chart_power, LV_OPA_0, LV_PART_ITEMS);
  energyObj.ser_power = lv_chart_add_series(
      energyObj.chart_power, lv_color_hex(kColGreen), LV_CHART_AXIS_PRIMARY_Y);
  energyObj.ser_aux = lv_chart_add_series(
      energyObj.chart_power, lv_color_hex(kColRed), LV_CHART_AXIS_SECONDARY_Y);
  for (int i = 0; i < kChartPoints; ++i) {
    lv_chart_set_value_by_id(energyObj.chart_power, energyObj.ser_power, i,
                             LV_CHART_POINT_NONE);
    lv_chart_set_value_by_id(energyObj.chart_power, energyObj.ser_aux, i, 0);
  }

  const int hourY = chartY + chartInnerH + 2;
  for (int h = 0; h < 24; ++h) {
    char buf[4];
    snprintf(buf, sizeof(buf), "%d", h + 1);
    s_lblHour[h] =
        makeLabel(s_powerPanel, chartX, hourY, chartInnerW / 24, buf, kColMuted);
    lv_obj_set_style_text_font(s_lblHour[h], kFontHour,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(s_lblHour[h], LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
  }

  s_utlumStrip = lv_obj_create(s_powerPanel);
  lv_obj_remove_style_all(s_utlumStrip);
  lv_obj_set_pos(s_utlumStrip, chartX, hourY + kHourH - 2);
  lv_obj_set_size(s_utlumStrip, chartInnerW, kUtlumStripH);
  lv_obj_set_style_bg_color(s_utlumStrip, lv_color_hex(0x2A2A30u),
                            LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_bg_opa(s_utlumStrip, LV_OPA_COVER,
                          LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_remove_flag(s_utlumStrip, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(s_utlumStrip, LV_OBJ_FLAG_CLICKABLE);

  layoutHourLabels(chartX, chartInnerW, hourY);

  s_monthPanel =
      makePanel(energyObj.screen, kMargin, topY + powerH + kGap, fullW, monthH);
  makeLabel(s_monthPanel, kPad, 6, fullW - 2 * kPad, "Topná sezóna září - květen",
            kColText);

  const int mChartY = 36;
  const int mNameH = 28;
  const int mChartH = monthH - mChartY - mNameH - kPad;
  s_monthNameY = monthH - mNameH;
  energyObj.chart_month = lv_chart_create(s_monthPanel);
  lv_obj_set_pos(energyObj.chart_month, kPad, mChartY);
  lv_obj_set_size(energyObj.chart_month, fullW - 2 * kPad, mChartH);
  lv_obj_set_style_bg_opa(energyObj.chart_month, LV_OPA_TRANSP,
                          LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_pad_all(energyObj.chart_month, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_border_width(energyObj.chart_month, 0,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
  // Mezera mezi bary — stejná geometrie pro popisky (LVGL pad_column)
  lv_obj_set_style_pad_column(energyObj.chart_month, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_pad_column(energyObj.chart_month, 0, LV_PART_ITEMS | LV_STATE_DEFAULT);
  lv_chart_set_type(energyObj.chart_month, LV_CHART_TYPE_BAR);
  lv_chart_set_point_count(energyObj.chart_month, ENERGY_SEASON_MONTHS);
  lv_chart_set_range(energyObj.chart_month, LV_CHART_AXIS_PRIMARY_Y, 0, 2000);
  energyObj.ser_month = lv_chart_add_series(
      energyObj.chart_month, lv_color_hex(kColAccent), LV_CHART_AXIS_PRIMARY_Y);

  for (int i = 0; i < ENERGY_SEASON_MONTHS; ++i) {
    s_lblMonthVal[i] = makeLabel(s_monthPanel, kPad, mChartY + 2, 40, "0",
                                 kColOrange);
    lv_obj_set_style_text_font(s_lblMonthVal[i], kFontSummary,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(s_lblMonthVal[i], LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    s_lblMonthName[i] =
        makeLabel(s_monthPanel, kPad, s_monthNameY, 40, kMonthNames[i],
                  kColMuted);
    lv_obj_set_style_text_align(s_lblMonthName[i], LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN | LV_STATE_DEFAULT);
  }
  layoutMonthLabels(mChartY + 2);

  energyObj.lbl_years =
      makeLabel(energyObj.screen, kMargin, kH - kMargin - yearsH + 4,
                kW - 2 * kMargin, "Sezóny: -", kColText);
  lv_obj_set_style_text_font(energyObj.lbl_years, kFontSummary,
                             LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_font(energyObj.lbl_day_kwh, kFontSummary,
                             LV_PART_MAIN | LV_STATE_DEFAULT);

  s_created = true;
  s_lastGen = 0;
  uiEnergyTick();
}

void uiEnergyEnsureCreated(void) {
  if (!s_created) {
    uiEnergyCreate();
  } else {
    // Po návratu na obrazovku vždy překreslit z RAM (NVS už načtená)
    s_lastGen = 0;
  }
}

void uiEnergyTick(void) {
  if (!s_created || !energyObj.screen) {
    return;
  }
  if (!uiIsEnergyScreen()) {
    return;
  }
  const uint32_t gen = climateEnergyHistoryGen();
  if (gen == s_lastGen) {
    refreshSummaryAndYears();
    layoutMonthLabels(-1);
    return;
  }
  s_lastGen = gen;
  refreshSummaryAndYears();
  refreshPowerChart();
  refreshMonthChart();
}

lv_obj_t* uiEnergyScreen(void) {
  uiEnergyEnsureCreated();
  return energyObj.screen;
}
