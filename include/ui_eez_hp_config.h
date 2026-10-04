#ifndef UI_EEZ_HP_CONFIG_H
#define UI_EEZ_HP_CONFIG_H

#include "lg_lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  lv_obj_t* screen;
  lv_obj_t* btn_back;
  lv_obj_t* lbl_title;
  lv_obj_t* btn_preset;
  lv_obj_t* btn_refresh;
  lv_obj_t* lbl_cfg_status;
  lv_obj_t* lbl_live;

  lv_obj_t* lbl_ctrl;
  lv_obj_t* lbl_curve;
  lv_obj_t* lbl_backup;

  lv_obj_t* lbl_ui_type;
  lv_obj_t* btn_ui_type_m;
  lv_obj_t* btn_ui_type_p;

  lv_obj_t* lbl_min_oat;
  lv_obj_t* btn_min_oat_m;
  lv_obj_t* btn_min_oat_p;

  lv_obj_t* lbl_ibh_oat;
  lv_obj_t* btn_ibh_oat_m;
  lv_obj_t* btn_ibh_oat_p;

  lv_obj_t* lbl_ibh_warm;
  lv_obj_t* btn_ibh_warm_m;
  lv_obj_t* btn_ibh_warm_p;

  lv_obj_t* lbl_ibh_dt;
  lv_obj_t* btn_ibh_dt_m;
  lv_obj_t* btn_ibh_dt_p;

  lv_obj_t* lbl_pump_dt;
  lv_obj_t* btn_pump_dt_m;
  lv_obj_t* btn_pump_dt_p;
} hp_config_objects_t;

extern hp_config_objects_t hpConfigObj;

void uiHpConfigCreate(void);
void uiHpConfigEnsureCreated(void);
void uiHpConfigOnLeave(void);
void uiHpConfigTick(void);
lv_obj_t* uiHpConfigScreen(void);

#ifdef __cplusplus
}
#endif

#endif
