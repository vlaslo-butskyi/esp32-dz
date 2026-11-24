// ДЗ 6.3 на ESP32-S3: кілька датчиків -> одна черга -> один consumer.
//
//   temp   (задача, ядро 0) - вбудований датчик температури, кожні 1000 мс;
//   pot    (задача, ядро 0) - потенціометр на GPIO1, середнє з 16 вимірювань, кожні 250 мс;
//   button (ISR)            - кнопка BOOT (GPIO0), подія на кожне натискання;
//   consumer (задача, ядро 1) - єдиний читач черги і єдиний власник статистики.
//
// Дані між задачами йдуть тільки через чергу: кожне джерело кладе в неї КОПІЮ повідомлення.
// Облік повідомлень, фільтр натискань і рядки логу - у ../common, спільні з STM32-версією.

#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "driver/gpio.h"
#include "driver/temperature_sensor.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "consumer.h"
#include "press_filter.h"
#include "sensor_msg.h"

static const char *TAG = "queue";

// Потенціометр: крайні виводи на 3V3 і GND, повзунок на GPIO1 = ADC1_CH0.
static const adc_unit_t    POT_UNIT    = ADC_UNIT_1;
static const adc_channel_t POT_CHANNEL = ADC_CHANNEL_0;
static const adc_atten_t   POT_ATTEN   = ADC_ATTEN_DB_12;  // діапазон до ~3,1 В
static const gpio_num_t    BUTTON_PIN  = GPIO_NUM_0;       // BOOT, натиснута = 0

enum {
  POT_SAMPLES = 16,
  // Експеримент «повільний consumer»: 0 - звичайна робота, 500 - черга переповниться.
  CONSUMER_DELAY_MS = 0,
  PRIO_PRODUCER = 2,
  PRIO_CONSUMER = 3,
  CORE_PRODUCER = 0,
  CORE_CONSUMER = 1,
  STACK         = 3072,
};

_Static_assert(PRIO_PRODUCER < PRIO_CONSUMER, "consumer must outrank producers");

static QueueHandle_t queue;  // створюється першим, до датчиків, задач і ISR

static uint32_t now_ms(void)
{
  return pdTICKS_TO_MS(xTaskGetTickCount());
}

// ---------------------------------------------------------------------------
// Датчики
// ---------------------------------------------------------------------------
static temperature_sensor_handle_t tsens;
static adc_oneshot_unit_handle_t   adc;
static adc_cali_handle_t           adc_cali;

static esp_err_t read_temp(int32_t *out)
{
  float celsius = 0;
  esp_err_t err = temperature_sensor_get_celsius(tsens, &celsius);
  if (err == ESP_OK) {
    *out = (int32_t)lroundf(celsius * 100);
  }
  return err;
}

static esp_err_t read_pot(int32_t *out)
{
  int sum = 0;
  for (int i = 0; i < POT_SAMPLES; ++i) {
    int raw = 0;
    esp_err_t err = adc_oneshot_read(adc, POT_CHANNEL, &raw);
    if (err != ESP_OK) {
      return err;
    }
    sum += raw;
  }
  // Калібрування з eFuse переводить код АЦП у мВ з урахуванням нелінійності.
  int mv = 0;
  esp_err_t err = adc_cali_raw_to_voltage(adc_cali, sum / POT_SAMPLES, &mv);
  if (err == ESP_OK) {
    *out = mv;
  }
  return err;
}

static void sensors_init(void)
{
  temperature_sensor_config_t tsens_cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
  ESP_ERROR_CHECK(temperature_sensor_install(&tsens_cfg, &tsens));
  ESP_ERROR_CHECK(temperature_sensor_enable(tsens));

  adc_oneshot_unit_init_cfg_t unit_cfg = {.unit_id = POT_UNIT};
  ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &adc));

  adc_oneshot_chan_cfg_t chan_cfg = {.atten = POT_ATTEN, .bitwidth = ADC_BITWIDTH_DEFAULT};
  ESP_ERROR_CHECK(adc_oneshot_config_channel(adc, POT_CHANNEL, &chan_cfg));

  adc_cali_curve_fitting_config_t cali_cfg = {
    .unit_id  = POT_UNIT,
    .chan     = POT_CHANNEL,
    .atten    = POT_ATTEN,
    .bitwidth = ADC_BITWIDTH_DEFAULT,
  };
  ESP_ERROR_CHECK(adc_cali_create_scheme_curve_fitting(&cali_cfg, &adc_cali));
}

// ---------------------------------------------------------------------------
// Продюсери: одна функція на всі датчики, параметр - рядок таблиці.
// ---------------------------------------------------------------------------
typedef struct {
  source_t source;
  esp_err_t (*read)(int32_t *out);
} producer_t;

static const producer_t PRODUCERS[] = {
  {SOURCE_TEMP, read_temp},
  {SOURCE_POT,  read_pot},
};

static void producer_task(void *arg)
{
  const producer_t    *p      = arg;
  const source_info_t *info   = &SOURCES[p->source];
  const TickType_t     period = pdMS_TO_TICKS(info->period_ms);
  TickType_t last_wake = xTaskGetTickCount();
  uint32_t   seq = 0;

  for (;;) {
    vTaskDelayUntil(&last_wake, period);

    int32_t value = 0;
    esp_err_t err = p->read(&value);
    if (err != ESP_OK) {
      printf("%7" PRIu32 "  %-6s read error: %s\n", now_ms(), info->name, esp_err_to_name(err));
      continue;
    }

    sensor_msg_t msg = {
      .source  = p->source,
      .core    = (uint8_t)xPortGetCoreID(),
      .seq     = seq++,  // росте і при невдалій відправці: consumer побачить розрив
      .time_ms = pdTICKS_TO_MS(last_wake),
      .value   = value,
    };

    // Обмежений таймаут: якщо consumer завис, датчик не зупиняється, а втрата видна в лозі.
    if (xQueueSend(queue, &msg, pdMS_TO_TICKS(SEND_TIMEOUT_MS)) != pdPASS) {
      printf("%7" PRIu32 "  %-6s queue full, #%" PRIu32 " dropped\n", now_ms(), info->name, msg.seq);
    }
  }
}

// ---------------------------------------------------------------------------
// ISR кнопки. Тільки ...FromISR API, без printf.
// ---------------------------------------------------------------------------

// Єдина змінна поза чергою: ISR не може надрукувати «queue full», тому рахує втрати тут.
// Пише тільки ISR, читає тільки consumer. Запис і читання 32 біт на ESP32-S3 атомарні.
static volatile uint32_t button_dropped;

static press_filter_t button_filter;  // чіпає тільки ISR після button_init
static uint32_t       button_seq;

// Без IRAM_ATTR: сервіс GPIO встановлено без ESP_INTR_FLAG_IRAM, тому на час запису у flash
// переривання чекає, а gpio_get_level і press_filter_edge з flash безпечні.
static void button_isr(void *arg)
{
  (void)arg;
  uint32_t now = pdTICKS_TO_MS(xTaskGetTickCountFromISR());
  bool released = gpio_get_level(BUTTON_PIN) != 0;
  if (!press_filter_edge(&button_filter, released, now)) {
    return;
  }

  sensor_msg_t msg = {
    .source  = SOURCE_BUTTON,
    .core    = (uint8_t)xPortGetCoreID(),
    .seq     = button_seq++,
    .time_ms = now,
  };

  // ISR не може чекати: якщо черга повна, подія губиться. Розрив у seq consumer побачить
  // лише на наступному натисканні, тому ISR ще й рахує втрату в button_dropped.
  BaseType_t woken = pdFALSE;
  if (xQueueSendFromISR(queue, &msg, &woken) != pdPASS) {
    button_dropped = button_dropped + 1;
  }
  // Якщо ISR розбудив задачу з вищим пріоритетом, перемкнутися одразу після виходу з ISR.
  portYIELD_FROM_ISR(woken);
}

static void button_init(void)
{
  gpio_config_t cfg = {
    .pin_bit_mask = 1ULL << BUTTON_PIN,
    .mode         = GPIO_MODE_INPUT,
    .pull_up_en   = GPIO_PULLUP_ENABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type    = GPIO_INTR_ANYEDGE,
  };
  ESP_ERROR_CHECK(gpio_config(&cfg));
  press_filter_init(&button_filter, gpio_get_level(BUTTON_PIN) != 0);
  ESP_ERROR_CHECK(gpio_install_isr_service(0));
  ESP_ERROR_CHECK(gpio_isr_handler_add(BUTTON_PIN, button_isr, NULL));
}

// ---------------------------------------------------------------------------
// Consumer
// ---------------------------------------------------------------------------
static void log_print(const char *line, void *ctx)
{
  (void)ctx;
  printf("%7" PRIu32 "  %s\n", now_ms(), line);
}

static void consumer_task(void *arg)
{
  (void)arg;
  // Статистику бачить тільки ця задача, тому гонки немає без жодного mutex.
  consumer_t c;
  consumer_init(&c, now_ms(), log_print, NULL);

  for (;;) {
    sensor_msg_t msg;
    if (xQueueReceive(queue, &msg, pdMS_TO_TICKS(RECV_TIMEOUT_MS)) == pdPASS) {
      consumer_note_depth(&c, uxQueueMessagesWaiting(queue) + 1);  // разом із щойно взятим
      consumer_handle(&c, &msg, now_ms());
      if (CONSUMER_DELAY_MS > 0) {
        vTaskDelay(pdMS_TO_TICKS(CONSUMER_DELAY_MS));
      }
    }
    consumer_tick(&c, now_ms(), button_dropped);
  }
}

// ---------------------------------------------------------------------------
// Старт: черга -> датчики -> consumer -> продюсери -> переривання.
// Кожен крок спирається на попередній: продюсер або ISR до створення черги відправив би в NULL.
// ---------------------------------------------------------------------------
static void start(TaskFunction_t fn, const char *name, void *arg, UBaseType_t prio, BaseType_t core)
{
  if (xTaskCreatePinnedToCore(fn, name, STACK, arg, prio, NULL, core) != pdPASS) {
    ESP_LOGE(TAG, "no memory for task %s", name);
    abort();
  }
}

void app_main(void)
{
  queue = xQueueCreate(QUEUE_LENGTH, sizeof(sensor_msg_t));
  if (queue == NULL) {
    ESP_LOGE(TAG, "no memory for queue");
    abort();
  }

  sensors_init();

  start(consumer_task, "consumer", NULL, PRIO_CONSUMER, CORE_CONSUMER);

  for (size_t i = 0; i < sizeof PRODUCERS / sizeof PRODUCERS[0]; ++i) {
    start(producer_task, SOURCES[PRODUCERS[i].source].name, (void *)&PRODUCERS[i], PRIO_PRODUCER,
          CORE_PRODUCER);
  }

  button_init();

  ESP_LOGI(TAG, "temp, pot on core %d -> queue of %d -> consumer on core %d, BOOT button via ISR",
           CORE_PRODUCER, QUEUE_LENGTH, CORE_CONSUMER);
}
