#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sched";

struct Config {
  static constexpr UBaseType_t PRIO_LOW      = 1;
  static constexpr UBaseType_t PRIO_NORMAL   = 2;
  static constexpr UBaseType_t PRIO_HIGH     = 3;
  static constexpr UBaseType_t PRIO_REALTIME = 4;

  static constexpr uint32_t SENSOR_PERIOD_MS      = 500;
  static constexpr uint32_t EVENT_PERIOD_MS       = 3000;
  static constexpr uint32_t MAINTENANCE_PERIOD_MS = 10000;
  static constexpr uint32_t MAINTENANCE_PAUSE_MS  = 2000;

  static constexpr uint32_t   STACK = 3072;
  static constexpr BaseType_t CORE  = 1;
};

static unsigned long now_ms(void)
{
  return pdTICKS_TO_MS(xTaskGetTickCount());
}

// Task 1, Normal: опитування датчика строго кожні 500 мс.
static void sensor_task(void *)
{
  const TickType_t period = pdMS_TO_TICKS(Config::SENSOR_PERIOD_MS);
  TickType_t last_wake = xTaskGetTickCount();

  for (;;) {
    vTaskDelayUntil(&last_wake, period);

    TickType_t late = xTaskGetTickCount() - last_wake;
    last_wake += late - late % period;

    printf("%7lu  [Sensor] Data read...\n", now_ms());
  }
}

// Task 3, High: живе рівно одну подію.
static void worker_task(void *)
{
  printf("%7lu  [Worker] Processing event...\n", now_ms());
  vTaskDelete(nullptr);
}

// Task 2, Low: раз на 3 с створює Worker Task.
static void event_task(void *)
{
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(Config::EVENT_PERIOD_MS));

    // Worker має вищий пріоритет, тому стартує ще до повернення з виклику.
    if (xTaskCreatePinnedToCore(
          worker_task, 
          "worker", 
          Config::STACK, 
          nullptr,
          Config::PRIO_HIGH, 
          nullptr, 
          Config::CORE
        ) != pdPASS) {
      printf("%7lu  [Event] no memory for Worker Task\n", now_ms());
    }
  }
}

// Task 4, Realtime: раз на 10 с ставить датчик на паузу на 2 с.
static void maintenance_task(void *arg)
{
  TaskHandle_t sensor = static_cast<TaskHandle_t>(arg);
  TickType_t last_wake = xTaskGetTickCount();

  for (;;) {
    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(Config::MAINTENANCE_PERIOD_MS));

    vTaskSuspend(sensor);
    printf("%7lu  [Maintenance] System paused for %lu seconds.\n", now_ms(),
           (unsigned long)(Config::MAINTENANCE_PAUSE_MS / 1000));
    vTaskDelay(pdMS_TO_TICKS(Config::MAINTENANCE_PAUSE_MS));
    vTaskResume(sensor);
  }
}

static void start(TaskFunction_t fn, const char *name, UBaseType_t prio, void *arg, TaskHandle_t *handle)
{
  if (xTaskCreatePinnedToCore(fn, name, Config::STACK, arg, prio, handle, Config::CORE) != pdPASS) {
    ESP_LOGE(TAG, "no memory for task %s", name);
    abort();
  }
}

extern "C" void app_main(void)
{
  TaskHandle_t sensor = nullptr;
  start(sensor_task,      "sensor",      Config::PRIO_NORMAL,   nullptr, &sensor);
  start(event_task,       "event",       Config::PRIO_LOW,      nullptr, nullptr);
  start(maintenance_task, "maintenance", Config::PRIO_REALTIME, sensor,  nullptr);

  ESP_LOGI(TAG, "sensor, event, maintenance started on core %d", int(Config::CORE));
}
