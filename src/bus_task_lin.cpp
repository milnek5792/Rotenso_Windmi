#include "bus_task_lin.h"
#include "bus_lg_config.h"
#include "bus_rotenso_config.h"
#include "src/bus/bus_rotenso_modbus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <Arduino.h>

static bool s_linStarted = false;

#if !LG_LIN_IN_LOOP

static TaskHandle_t s_linTask = nullptr;

static void lgTaskLin(void* param) {
  (void)param;
  rotensoBusInit();
  Serial.printf("[MB] task bezi na core %d\n", xPortGetCoreID());

  for (;;) {
    rotensoBusTick();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

#endif

void lgTaskLinStart() {
  if (s_linStarted) { return; }

#if LG_LIN_IN_LOOP
  s_linStarted = true;
  Serial.println("[MB] odposlech v loop() — UART jiz init v setup()");
#else
  BaseType_t ok = xTaskCreatePinnedToCore(
      lgTaskLin, "mb_bus", LG_TASK_LIN_STACK, nullptr, LG_TASK_LIN_PRIO,
      &s_linTask, LG_CORE_LIN);
  if (ok != pdPASS) {
    Serial.println("[MB] CHYBA: nelze vytvorit task (nedostatek pameti?)");
    return;
  }

  s_linStarted = true;
  Serial.printf("[MB] task vytvoren (core %d, stack %u B)\n", LG_CORE_LIN,
                (unsigned)LG_TASK_LIN_STACK);
#endif
}

bool lgTaskLinIsRunning() {
  return s_linStarted;
}
