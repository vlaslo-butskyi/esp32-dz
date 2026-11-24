// ДЗ 6.5: конвеєр на двох ядрах. Спільні ресурси, ISR і аварійна зупинка.
//
//   part_isr  (ISR, кнопка GPIO 16) - датчик деталі: лічильник parts += 1;
//   stop_isr  (ISR, кнопка BOOT)    - аварійна зупинка: будить задачу estop;
//   report (Task 1, ядро 0) - раз на секунду: лічильник -> швидкість, деталі/хв -> буфер звіту;
//   adc    (Task 2, ядро 1) - після кожного звіту Task 1: АЦП -> дописати в той самий буфер;
//   estop  (Task 3, ядро 0, найвищий пріоритет) - спить до stop_isr, потім зупиняє конвеєр.
//
// Спільні дані і чим їх захищено:
//   parts       - пише ISR, читають задачі         -> спінлок (mutex у ISR брати не можна);
//   report_text - пишуть три задачі на двох ядрах   -> mutex з успадкуванням пріоритету;
//   halted      - пише estop один раз, читають report і adc під тим самим mutex.
// Порядок «Task 1 пише, потім Task 2 дописує» задає бінарний семафор report_ready.
//
// Фільтр брязкоту, швидкість і текст звіту - у ../common, спільні з STM32-версією.

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "press_filter.h"
#include "rate.h"
#include "report.h"

#if CONFIG_FREERTOS_UNICORE
#error "ДЗ потребує обох ядер: вимкніть CONFIG_FREERTOS_UNICORE"
#endif

// Датчик деталі: тактова кнопка, другий контакт на GND. Натиснута = 0.
#define PART_PIN GPIO_NUM_16
// Аварійна зупинка: кнопка BOOT на платі (GPIO 0, на платі є підтяжка до живлення).
#define STOP_PIN GPIO_NUM_0
// Індикатор тривоги: світлодіод через резистор 220 Ом, катод на GND.
#define LED_PIN  GPIO_NUM_4

// Потенціометр: крайні виводи на 3V3 і GND, повзунок на GPIO 1 = ADC1_CH0.
#define POT_UNIT    ADC_UNIT_1
#define POT_CHANNEL ADC_CHANNEL_0
#define POT_ATTEN   ADC_ATTEN_DB_12  // діапазон до ~3,1 В
#define POT_SAMPLES 16

#define REPORT_PERIOD_MS UINT32_C(1000)
#define READY_TIMEOUT_MS UINT32_C(1500)  // adc: звіт Task 1 не прийшов
#define MUTEX_TIMEOUT_MS UINT32_C(100)

#define PRIO_REPORT ((UBaseType_t)5)
#define PRIO_ADC    ((UBaseType_t)5)
#define PRIO_ESTOP  ((UBaseType_t)10)
// estop на ядрі 0, там само, де сервіс переривань GPIO (його встановлює app_main).
// Тоді portYIELD_FROM_ISR перемикає саме те ядро, на якому estop чекає.
#define CORE_REPORT ((BaseType_t)0)
#define CORE_ADC    ((BaseType_t)1)
#define CORE_ESTOP  ((BaseType_t)0)
#define STACK_BYTES UINT32_C(3072)

_Static_assert(READY_TIMEOUT_MS > REPORT_PERIOD_MS, "adc must wait longer than one report period");
_Static_assert(MUTEX_TIMEOUT_MS < REPORT_PERIOD_MS, "a mutex timeout must fit into one report period");

// ---------------------------------------------------------------------------
// Спільні дані.
// ---------------------------------------------------------------------------
static portMUX_TYPE      parts_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t parts;  // лише під parts_lock

static SemaphoreHandle_t report_mutex;
static StaticSemaphore_t report_mutex_buf;
static char              report_text[REPORT_SIZE];  // лише під report_mutex
// Пише тільки estop, один раз і до спроби взяти mutex: зупинка не чекає на mutex.
// report і adc читають прапорець під mutex, разом із записом буфера.
static volatile bool     halted;

static SemaphoreHandle_t report_ready;  // report -> adc: «рядок записано, дописуйте»
static StaticSemaphore_t report_ready_buf;
static SemaphoreHandle_t stop_signal;   // stop_isr -> estop
static StaticSemaphore_t stop_signal_buf;

// Час першого фронту BOOT. Пише ISR, читає estop після семафора.
static volatile int64_t stop_time_us;

static press_filter_t part_filter;  // чіпає тільки part_isr після gpio_init

static adc_oneshot_unit_handle_t adc;  // тільки задача adc
static adc_cali_handle_t         adc_cali;

// ---------------------------------------------------------------------------
// ISR. Лише час, рівень піна, лічильник і семафор. Лог і обчислення - у задачах.
// Без IRAM_ATTR: сервіс GPIO встановлено без ESP_INTR_FLAG_IRAM, тому на час запису
// у flash переривання чекає, а press_filter_edge з flash безпечна.
// ---------------------------------------------------------------------------
static void part_isr(void *arg)
{
  (void)arg;
  uint32_t now_ms   = (uint32_t)(esp_timer_get_time() / 1000);
  bool     released = gpio_get_level(PART_PIN) != 0;
  if (press_filter_edge(&part_filter, released, now_ms)) {
    portENTER_CRITICAL_ISR(&parts_lock);
    parts = parts + 1;  // Read-Modify-Write, тому під спінлоком
    portEXIT_CRITICAL_ISR(&parts_lock);
  }
}

static void stop_isr(void *arg)
{
  (void)arg;
  if (stop_time_us == 0) {
    stop_time_us = esp_timer_get_time();  // брязкіт не переписує час першого фронту
  }
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(stop_signal, &woken);
  portYIELD_FROM_ISR(woken);  // estop отримає процесор одразу після виходу з ISR
}

static uint32_t read_parts(void)
{
  portENTER_CRITICAL(&parts_lock);
  uint32_t n = parts;
  portEXIT_CRITICAL(&parts_lock);
  return n;
}

// ---------------------------------------------------------------------------
// Task 1 (ядро 0): лічильник -> швидкість -> початок рядка звіту.
// ---------------------------------------------------------------------------
static void report_task(void *arg)
{
  (void)arg;
  rate_t     rate;
  TickType_t wake = xTaskGetTickCount();
  rate_init(&rate, read_parts());

  for (;;) {
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(REPORT_PERIOD_MS));

    uint32_t count    = read_parts();
    uint32_t rate_x10 = rate_update(&rate, count);

    // Рядок формується до mutex: критична секція - лише копіювання.
    char line[REPORT_SIZE];
    report_begin(line, sizeof line, count, rate_x10);

    if (xSemaphoreTake(report_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
      ESP_LOGW("report", "mutex timeout, report skipped");
      continue;
    }
    // Перевірка halted і запис - в одній критичній секції. Інакше estop міг би записати
    // SYSTEM HALTED! між перевіркою і записом, і звіт переписав би його.
    bool stop = halted;
    if (!stop) {
      memcpy(report_text, line, sizeof line);
    }
    xSemaphoreGive(report_mutex);

    if (stop) {
      ESP_LOGW("report", "conveyor halted, task stopped");
      vTaskDelete(NULL);
    }
    xSemaphoreGive(report_ready);
  }
}

// ---------------------------------------------------------------------------
// Task 2 (ядро 1): АЦП -> кінець рядка звіту -> вивід.
// ---------------------------------------------------------------------------
static int32_t read_pot_mv(void)
{
  int sum = 0;
  for (int i = 0; i < POT_SAMPLES; i++) {
    int raw = 0;
    ESP_ERROR_CHECK(adc_oneshot_read(adc, POT_CHANNEL, &raw));
    sum += raw;
  }
  int mv = 0;
  ESP_ERROR_CHECK(adc_cali_raw_to_voltage(adc_cali, sum / POT_SAMPLES, &mv));
  return mv;
}

static void adc_task(void *arg)
{
  (void)arg;
  for (;;) {
    bool fresh = xSemaphoreTake(report_ready, pdMS_TO_TICKS(READY_TIMEOUT_MS)) == pdTRUE;

    // АЦП і форматування - до mutex. 16 вимірів довші за копіювання рядка.
    char tail[TAIL_SIZE];
    report_pot(tail, sizeof tail, read_pot_mv());

    if (xSemaphoreTake(report_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
      ESP_LOGW("adc", "mutex timeout, ADC value skipped");
      continue;
    }
    bool stop     = halted;
    bool appended = !stop && fresh && report_append(report_text, sizeof report_text, tail);
    char copy[REPORT_SIZE];
    memcpy(copy, report_text, sizeof copy);
    xSemaphoreGive(report_mutex);

    // Вивід у консоль повільний, тому він після Give, з копії буфера.
    if (stop) {
      ESP_LOGW("adc", "conveyor halted, task stopped");
      vTaskDelete(NULL);
    }
    if (!fresh) {
      ESP_LOGW("adc", "no report from Task 1 for %" PRIu32 " ms", READY_TIMEOUT_MS);
    } else if (!appended) {
      ESP_LOGW("adc", "report buffer full, ADC value skipped");
    } else {
      ESP_LOGI("report", "%s", copy);
    }
  }
}

// ---------------------------------------------------------------------------
// Task 3 (ядро 0, найвищий пріоритет): спить на семафорі і не займає процесор.
// ---------------------------------------------------------------------------
static void estop_task(void *arg)
{
  (void)arg;
  // portMAX_DELAY тут правильний: без події у задачі немає роботи.
  xSemaphoreTake(stop_signal, portMAX_DELAY);
  int64_t woke_us = esp_timer_get_time();

  // Зупинка не залежить від mutex: світлодіод, переривання і прапорець - першими.
  gpio_set_level(LED_PIN, 1);
  gpio_intr_disable(PART_PIN);  // датчик більше не лічить деталі
  gpio_intr_disable(STOP_PIN);
  halted = true;  // report і adc зупиняться на своїй наступній критичній секції

  // Якщо mutex тримає report або adc, FreeRTOS піднімає її пріоритет до 10, поки
  // вона не віддасть mutex (успадкування пріоритету).
  char copy[REPORT_SIZE];
  bool written = xSemaphoreTake(report_mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE;
  if (written) {
    snprintf(report_text, sizeof report_text, "%s", REPORT_HALTED);
    memcpy(copy, report_text, sizeof copy);
    xSemaphoreGive(report_mutex);
  }
  int64_t done_us = esp_timer_get_time();

  if (written) {
    ESP_LOGE("report", "%s", copy);
  } else {
    ESP_LOGE("estop", "mutex timeout, " REPORT_HALTED " is not in the report buffer");
  }
  ESP_LOGW("estop", "parts %" PRIu32 ", ISR -> estop %" PRId64 " us, mutex %" PRId64 " us",
           read_parts(), woke_us - stop_time_us, done_us - woke_us);
  vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Старт: GPIO і АЦП -> mutex і семафори -> задачі -> переривання.
// Якщо переривання увімкнути раніше, BOOT під час старту дав би Give на NULL.
// ---------------------------------------------------------------------------
static void gpio_init(void)
{
  gpio_config_t part = {
    .pin_bit_mask = 1ULL << PART_PIN,
    .mode         = GPIO_MODE_INPUT,
    .pull_up_en   = GPIO_PULLUP_ENABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type    = GPIO_INTR_ANYEDGE,  // фільтр брязкоту дивиться на обидва фронти
  };
  ESP_ERROR_CHECK(gpio_config(&part));
  press_filter_init(&part_filter, gpio_get_level(PART_PIN) != 0);

  gpio_config_t stop = part;
  stop.pin_bit_mask  = 1ULL << STOP_PIN;
  stop.intr_type     = GPIO_INTR_NEGEDGE;  // натискання BOOT = перехід 1 -> 0
  ESP_ERROR_CHECK(gpio_config(&stop));

  gpio_config_t led = {
    .pin_bit_mask = 1ULL << LED_PIN,
    .mode         = GPIO_MODE_OUTPUT,
    .pull_up_en   = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type    = GPIO_INTR_DISABLE,
  };
  ESP_ERROR_CHECK(gpio_config(&led));
  ESP_ERROR_CHECK(gpio_set_level(LED_PIN, 0));
}

static void adc_init(void)
{
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

static void start(TaskFunction_t fn, const char *name, UBaseType_t prio, BaseType_t core)
{
  if (xTaskCreatePinnedToCore(fn, name, STACK_BYTES, NULL, prio, NULL, core) != pdPASS) {
    ESP_LOGE("conveyor", "no memory for task %s", name);
    abort();
  }
}

void app_main(void)
{
  gpio_init();
  adc_init();

  // Static: пам'ять - глобальні змінні, тому створення не може не вдатися.
  report_mutex = xSemaphoreCreateMutexStatic(&report_mutex_buf);
  report_ready = xSemaphoreCreateBinaryStatic(&report_ready_buf);
  stop_signal  = xSemaphoreCreateBinaryStatic(&stop_signal_buf);

  start(estop_task, "estop", PRIO_ESTOP, CORE_ESTOP);
  start(adc_task, "adc", PRIO_ADC, CORE_ADC);
  start(report_task, "report", PRIO_REPORT, CORE_REPORT);

  ESP_ERROR_CHECK(gpio_install_isr_service(0));
  ESP_ERROR_CHECK(gpio_isr_handler_add(PART_PIN, part_isr, NULL));
  ESP_ERROR_CHECK(gpio_isr_handler_add(STOP_PIN, stop_isr, NULL));

  ESP_LOGI("conveyor", "part sensor GPIO %d, emergency stop BOOT, alarm LED GPIO %d", PART_PIN,
           LED_PIN);
}
