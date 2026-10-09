// bus_lg_model.h — sdílený stav mezi sběrnicí TC (Modbus) a UI (mutex)
#ifndef LG_MODEL_H
#define LG_MODEL_H

#include <Arduino.h>

extern uint8_t mCilova, mVstupni, mVystupni;
/** Teploty z Modbus (°C, 0.1 rozlišení); platné když lgMaCerstoA0(). */
extern float mVenkovniC, mVstupniC, mVystupniC;
extern bool mVenkovniOk, mVodaOk;
extern bool mVstupniOk, mVystupniOk;
/** Running mode z 002DH (0=Off, 2=Heat, 7=Defrost, …). */
extern uint16_t mMbRunningMode;
/** Čekáme na potvrzení START/STOP přes 002DH. */
extern bool mMbPowerPending;
extern bool mMbPowerWantOn;
extern bool stavZapnuto;
extern volatile bool pozadavekNaZapis;
extern bool pozadavekZmenaStartu;
extern uint8_t novaCilovaTeplota;
extern bool bliknuti;
extern bool monitorPozastaven;
extern bool origOvladacDetekovan;
/** Tichý režim z orig. wall unitu (C0/A0 subcmd 0x31, normální 0x32). */
extern bool origTichyRezimLin;
extern bool soloRezimTab5;
/** PARALLEL: Tab + drátový ovladač na jedné lince. */
extern bool parallelRezimTab5;
extern bool drzetStavAktivni;
extern bool cilovyZapnutoTab5;
extern bool cekameNaOrigStart;
extern bool tcPozadavekZap;
extern uint8_t cilovaTeplotaTab5;
extern char posledniStavovyText[64];
extern volatile bool potrebaObnovitDisplej;

void lgModelInit();
void lgModelLock();
void lgModelUnlock();

void nastavStavovyText(const char* text);

void lgModelSnapA0(const uint8_t* data, uint8_t len);
void lgModelSnapA0Locked(const uint8_t* data, uint8_t len);
uint8_t lgModelA0Bajt(uint8_t idx, uint8_t vychozi = 0);
bool lgMaCerstoA0(uint32_t maxAgeMs = 0);

/** Obnoví „live“ spojení s TC (Modbus poll OK). */
void lgModelTouchLive(void);
/**
 * Teploty z Windmi Modbus (0001 / 0003 / 0004).
 * Inlet/outlet se propíší nezávisle (ne jen když jsou platné obě).
 */
void lgModelSetMbTemps(float outdoorC, bool outdoorOk, float inletC, bool inletOk,
                       float outletC, bool outletOk);
/**
 * Stav TČ z Modbus → syntetické A0 bity (čerpadlo/kompresor/odmrazování/tichý)
 * + live timestamp. El. topení: pouze 0081H bit0 (IBH1).
 */
void lgModelSetMbStatus(uint16_t settingMode, uint16_t runningMode,
                        int16_t compFreqX10, uint16_t pumpSpeed,
                        uint16_t quietNight, uint16_t loadOutput,
                        bool loadOutputOk, uint16_t waterFlowX100);
/** SP vody z Modbus (0191H / 0033H) → mCilova + a0Snap[8] pro UI confirm. */
void lgModelSetMbWaterSp(uint8_t spC);

/** Live diagnostika z pollStatus (+ volitelně 100FH). */
typedef struct {
  bool valid;
  uint16_t setting_mode;
  uint16_t running_mode;
  int16_t comp_freq_x10;
  uint16_t pump_speed;
  uint16_t quiet_night;
  uint16_t load_output;
  bool load_ok;
  uint16_t water_flow_x100;
  int16_t req_comp_freq_x10;
  bool req_comp_ok;
} WindmiLiveSnap;

/** RW konfigurační registry (bez TUV, 1× IBH). */
typedef struct {
  bool valid;
  uint16_t ctrl_mode;       /**< 100DH: 0=voda, 1=okolí */
  int16_t curve_type;       /**< 0245H: −1=pevný SP */
  uint16_t backup_heater;   /**< 0259H: 6=vnitřní EH */
  int16_t min_oat_heat_x10; /**< 0202H */
  uint16_t ibh_warmup_min;  /**< 025AH */
  int16_t ibh_delta_t_x10;  /**< 025BH */
  int16_t ibh_oat_x10;      /**< 025CH */
  int16_t pump_delta_t_x10; /**< 0239H */
  uint16_t ui_type;         /**< 0209H: 1=kontakty, 2=WUI (0 TČ odmítá) */
  uint16_t mask;            /**< bit i = reg i přečten (0x100=ui_type) */
} WindmiHpConfigSnap;

void lgModelSetMbLiveExtras(int16_t reqCompFreqX10, bool reqCompOk);
void lgModelSetMbHpConfig(const WindmiHpConfigSnap* cfg);
void lgModelReadLiveSnap(WindmiLiveSnap* out);
void lgModelReadHpConfigSnap(WindmiHpConfigSnap* out);

/** Alarm bitmapy Windmi 1009H…100CH. */
typedef struct {
  bool valid;
  uint16_t bm[4];
} WindmiAlarmSnap;

void lgModelSetMbAlarms(const uint16_t bm[4]);
void lgModelReadAlarmSnap(WindmiAlarmSnap* out);
/** true pokud je alespoň jeden bit alarmu aktivní. */
bool lgModelHasHpAlarm(void);

typedef struct {
  bool lin_live;
  uint8_t b2;
  uint8_t b3;
  uint8_t a0_sp;
  uint8_t m_vstupni;
  uint8_t m_vystupni;
  uint8_t m_cilova;
  uint8_t nova_cilova;
  bool pozadavek_zapis;
  bool cilovy_zapnuto;
  bool cekame_orig;
  bool tc_pozadavek;
} LgModelUiSnap;

void lgModelReadUiSnap(LgModelUiSnap* out);
void lgModelRestoreSessionFromNvs(void);

#endif
