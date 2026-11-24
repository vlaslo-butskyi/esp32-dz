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

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_FREERTOS_UNICORE
#error "ДЗ потребує обох ядер: вимкніть CONFIG_FREERTOS_UNICORE"
#endif

struct Config {
  // Датчик деталі: тактова кнопка, другий контакт на GND. Натиснута = 0.
  static constexpr gpio_num_t PART_PIN = GPIO_NUM_16;
  // Аварійна зупинка: кнопка BOOT на платі (GPIO 0, на платі є підтяжка до живлення).
  static constexpr gpio_num_t STOP_PIN = GPIO_NUM_0;
  // Індикатор тривоги: світлодіод через резистор 220 Ом, катод на GND.
  static constexpr gpio_num_t LED_PIN = GPIO_NUM_4;
  static constexpr int RELEASED = 1;

  // Потенціометр: крайні виводи на 3V3 і GND, повзунок на GPIO 1 = ADC1_CH0.
  static constexpr adc_unit_t    POT_UNIT    = ADC_UNIT_1;
  static constexpr adc_channel_t POT_CHANNEL = ADC_CHANNEL_0;
  static constexpr adc_atten_t   POT_ATTEN   = ADC_ATTEN_DB_12;  // діапазон до ~3,1 В
  static constexpr int           POT_SAMPLES = 16;

  // Debounce в ISR: фронт починає нову деталь, якщо перед ним лінія мовчала 30 мс
  // і стояла у стані «відпущено».
  static constexpr int64_t PART_QUIET_US = 30 * 1000;

  static constexpr uint32_t REPORT_PERIOD_MS = 1000;
  static constexpr uint32_t READY_TIMEOUT_MS = 1500;  // adc: звіт Task 1 не прийшов
  static constexpr uint32_t MUTEX_TIMEOUT_MS = 100;
  static constexpr uint32_t RATE_WINDOW_S    = 10;    // швидкість за останні 10 с
  static constexpr size_t   REPORT_SIZE      = 64;

  static constexpr UBaseType_t PRIO_REPORT = 5;
  static constexpr UBaseType_t PRIO_ADC    = 5;
  static constexpr UBaseType_t PRIO_ESTOP  = 10;
  // estop на ядрі 0, там само, де сервіс переривань GPIO (його встановлює app_main).
  // Тоді portYIELD_FROM_ISR перемикає саме те ядро, на якому estop чекає.
  static constexpr BaseType_t CORE_REPORT = 0;
  static constexpr BaseType_t CORE_ADC    = 1;
  static constexpr BaseType_t CORE_ESTOP  = 0;
  static constexpr uint32_t   STACK       = 3072;
};

static_assert(Config::READY_TIMEOUT_MS > Config::REPORT_PERIOD_MS,
              "adc must wait longer than one report period");
static_assert(Config::MUTEX_TIMEOUT_MS < Config::REPORT_PERIOD_MS,
              "a mutex timeout must fit into one report period");
static_assert(Config::RATE_WINDOW_S > 0, "rate window must not be empty");

// ---------------------------------------------------------------------------
// Спільні дані.
// ---------------------------------------------------------------------------
static portMUX_TYPE      parts_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t parts;  // лише під parts_lock

static SemaphoreHandle_t report_mutex;
static StaticSemaphore_t report_mutex_buf;
static char              report_text[Config::REPORT_SIZE];  // лише під report_mutex
// Пише тільки estop, один раз і до спроби взяти mutex: зупинка не чекає на mutex.
// report і adc читають прапорець під mutex, разом із записом буфера.
static volatile bool     halted;

static SemaphoreHandle_t report_ready;  // report -> adc: «рядок записано, дописуйте»
static StaticSemaphore_t report_ready_buf;
static SemaphoreHandle_t stop_signal;   // stop_isr -> estop
static StaticSemaphore_t stop_signal_buf;

// Час першого фронту BOOT. Пише ISR, читає estop після семафора.
static volatile int64_t stop_time_us;

static adc_oneshot_unit_handle_t adc;  // тільки задача adc
static adc_cali_handle_t         adc_cali;

// ---------------------------------------------------------------------------
// ISR. Лише час, рівень піна, лічильник і семафор. Лог і обчислення - у задачах.
// ---------------------------------------------------------------------------
static void IRAM_ATTR part_isr(void *)
{
  // Ці дві змінні бачить тільки ISR, тому захист їм не потрібен.
  static int64_t last_edge_us = 0;
  static int     quiet_level  = Config::RELEASED;  // рівень після останнього фронту

  int64_t now   = esp_timer_get_time();
  int     level = gpio_get_level(Config::PART_PIN);

  // Нова деталь: лінія мовчала 30 мс і стояла «відпущено». Брязкіт при натисканні і при
  // відпусканні дає фронти частіше за 30 мс, тому другу деталь він не додає.
  if (now - last_edge_us >= Config::PART_QUIET_US && quiet_level == Config::RELEASED) {
    portENTER_CRITICAL_ISR(&parts_lock);
    parts = parts + 1;  // Read-Modify-Write, тому під спінлоком
    portEXIT_CRITICAL_ISR(&parts_lock);
  }
  // Після останнього фронту серії рівень більше не змінюється. Тому це значення є
  // рівнем лінії під час наступної паузи.
  last_edge_us = now;
  quiet_level  = level;
}

static void IRAM_ATTR stop_isr(void *)
{
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
static void report_task(void *)
{
  uint32_t   past[Config::RATE_WINDOW_S];  // лічильник 1..10 с тому, кільцевий буфер
  uint32_t   start   = read_parts();
  uint32_t   head    = 0;
  uint32_t   seconds = 0;
  TickType_t wake    = xTaskGetTickCount();
  for (uint32_t &p : past) {
    p = start;
  }

  for (;;) {
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(Config::REPORT_PERIOD_MS));

    uint32_t count = read_parts();
    if (seconds < Config::RATE_WINDOW_S) {
      seconds++;
    }
    uint32_t delta = count - past[head];  // деталі за останні `seconds` секунд
    past[head]     = count;
    head           = (head + 1) % Config::RATE_WINDOW_S;
    uint32_t rate_x10 = delta * 600 / seconds;  // деталі/хв × 10

    // Рядок формується до mutex: критична секція - лише копіювання.
    char line[Config::REPORT_SIZE];
    snprintf(line, sizeof(line), "parts %" PRIu32 ", rate %" PRIu32 ".%" PRIu32 "/min", count,
             rate_x10 / 10, rate_x10 % 10);

    if (xSemaphoreTake(report_mutex, pdMS_TO_TICKS(Config::MUTEX_TIMEOUT_MS)) != pdTRUE) {
      ESP_LOGW("report", "mutex timeout, report skipped");
      continue;
    }
    // Перевірка halted і запис - в одній критичній секції. Інакше estop міг би записати
    // SYSTEM HALTED! між перевіркою і записом, і звіт переписав би його.
    bool stop = halted;
    if (!stop) {
      memcpy(report_text, line, sizeof(line));
    }
    xSemaphoreGive(report_mutex);

    if (stop) {
      ESP_LOGW("report", "conveyor halted, task stopped");
      vTaskDelete(nullptr);
    }
    xSemaphoreGive(report_ready);
  }
}

// ---------------------------------------------------------------------------
// Task 2 (ядро 1): АЦП -> кінець рядка звіту -> вивід.
// ---------------------------------------------------------------------------
static int read_pot_mv(void)
{
  int sum = 0;
  for (int i = 0; i < Config::POT_SAMPLES; i++) {
    int raw = 0;
    ESP_ERROR_CHECK(adc_oneshot_read(adc, Config::POT_CHANNEL, &raw));
    sum += raw;
  }
  int mv = 0;
  ESP_ERROR_CHECK(adc_cali_raw_to_voltage(adc_cali, sum / Config::POT_SAMPLES, &mv));
  return mv;
}

static void adc_task(void *)
{
  for (;;) {
    bool fresh = xSemaphoreTake(report_ready, pdMS_TO_TICKS(Config::READY_TIMEOUT_MS)) == pdTRUE;

    // АЦП і форматування - до mutex. 16 вимірів довші за копіювання рядка.
    char tail[24];
    snprintf(tail, sizeof(tail), "; pot %d mV", read_pot_mv());

    if (xSemaphoreTake(report_mutex, pdMS_TO_TICKS(Config::MUTEX_TIMEOUT_MS)) != pdTRUE) {
      ESP_LOGW("adc", "mutex timeout, ADC value skipped");
      continue;
    }
    bool   stop     = halted;
    bool   appended = false;
    size_t len      = strlen(report_text);
    size_t add      = strlen(tail);
    if (!stop && fresh && len + add < sizeof(report_text)) {
      memcpy(report_text + len, tail, add + 1);
      appended = true;
    }
    char copy[Config::REPORT_SIZE];
    memcpy(copy, report_text, sizeof(copy));
    xSemaphoreGive(report_mutex);

    // Вивід у консоль повільний, тому він після Give, з копії буфера.
    if (stop) {
      ESP_LOGW("adc", "conveyor halted, task stopped");
      vTaskDelete(nullptr);
    }
    if (!fresh) {
      ESP_LOGW("adc", "no report from Task 1 for %" PRIu32 " ms", Config::READY_TIMEOUT_MS);
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
static void estop_task(void *)
{
  // portMAX_DELAY тут правильний: без події у задачі немає роботи.
  xSemaphoreTake(stop_signal, portMAX_DELAY);
  int64_t woke_us = esp_timer_get_time();

  // Зупинка не залежить від mutex: світлодіод, переривання і прапорець - першими.
  gpio_set_level(Config::LED_PIN, 1);
  gpio_intr_disable(Config::PART_PIN);  // датчик більше не лічить деталі
  gpio_intr_disable(Config::STOP_PIN);
  halted = true;  // report і adc зупиняться на своїй наступній критичній секції

  // Якщо mutex тримає report або adc, FreeRTOS піднімає її пріоритет до 10, поки
  // вона не віддасть mutex (успадкування пріоритету).
  char copy[Config::REPORT_SIZE];
  bool written = xSemaphoreTake(report_mutex, pdMS_TO_TICKS(Config::MUTEX_TIMEOUT_MS)) == pdTRUE;
  if (written) {
    snprintf(report_text, sizeof(report_text), "SYSTEM HALTED!");
    memcpy(copy, report_text, sizeof(copy));
    xSemaphoreGive(report_mutex);
  }
  int64_t done_us = esp_timer_get_time();

  if (written) {
    ESP_LOGE("report", "%s", copy);
  } else {
    ESP_LOGE("estop", "mutex timeout, SYSTEM HALTED! is not in the report buffer");
  }
  ESP_LOGW("estop", "parts %" PRIu32 ", ISR -> estop %" PRId64 " us, mutex %" PRId64 " us",
           read_parts(), woke_us - stop_time_us, done_us - woke_us);
  vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
// Старт: GPIO і АЦП -> mutex і семафори -> задачі -> переривання.
// Якщо переривання увімкнути раніше, BOOT під час старту дав би Give на NULL.
// ---------------------------------------------------------------------------
static void gpio_init(void)
{
  gpio_config_t part = {};
  part.pin_bit_mask  = 1ULL << Config::PART_PIN;
  part.mode          = GPIO_MODE_INPUT;
  part.pull_up_en    = GPIO_PULLUP_ENABLE;
  part.intr_type     = GPIO_INTR_ANYEDGE;  // debounce в ISR дивиться на обидва фронти
  ESP_ERROR_CHECK(gpio_config(&part));

  gpio_config_t stop = part;
  stop.pin_bit_mask  = 1ULL << Config::STOP_PIN;
  stop.intr_type     = GPIO_INTR_NEGEDGE;  // натискання BOOT = перехід 1 -> 0
  ESP_ERROR_CHECK(gpio_config(&stop));

  gpio_config_t led = {};
  led.pin_bit_mask  = 1ULL << Config::LED_PIN;
  led.mode          = GPIO_MODE_OUTPUT;
  ESP_ERROR_CHECK(gpio_config(&led));
  ESP_ERROR_CHECK(gpio_set_level(Config::LED_PIN, 0));
}

static void adc_init(void)
{
  adc_oneshot_unit_init_cfg_t unit_cfg = {};
  unit_cfg.unit_id = Config::POT_UNIT;
  ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &adc));

  adc_oneshot_chan_cfg_t chan_cfg = {};
  chan_cfg.atten    = Config::POT_ATTEN;
  chan_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
  ESP_ERROR_CHECK(adc_oneshot_config_channel(adc, Config::POT_CHANNEL, &chan_cfg));

  adc_cali_curve_fitting_config_t cali_cfg = {};
  cali_cfg.unit_id  = Config::POT_UNIT;
  cali_cfg.chan     = Config::POT_CHANNEL;
  cali_cfg.atten    = Config::POT_ATTEN;
  cali_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
  ESP_ERROR_CHECK(adc_cali_create_scheme_curve_fitting(&cali_cfg, &adc_cali));
}

static void start(TaskFunction_t fn, const char *name, UBaseType_t prio, BaseType_t core)
{
  if (xTaskCreatePinnedToCore(fn, name, Config::STACK, nullptr, prio, nullptr, core) != pdPASS) {
    ESP_LOGE("conveyor", "no memory for task %s", name);
    abort();
  }
}

extern "C" void app_main(void)
{
  gpio_init();
  adc_init();

  // Static: пам'ять - глобальні змінні, тому створення не може не вдатися.
  report_mutex = xSemaphoreCreateMutexStatic(&report_mutex_buf);
  report_ready = xSemaphoreCreateBinaryStatic(&report_ready_buf);
  stop_signal  = xSemaphoreCreateBinaryStatic(&stop_signal_buf);

  start(estop_task, "estop", Config::PRIO_ESTOP, Config::CORE_ESTOP);
  start(adc_task, "adc", Config::PRIO_ADC, Config::CORE_ADC);
  start(report_task, "report", Config::PRIO_REPORT, Config::CORE_REPORT);

  ESP_ERROR_CHECK(gpio_install_isr_service(0));
  ESP_ERROR_CHECK(gpio_isr_handler_add(Config::PART_PIN, part_isr, nullptr));
  ESP_ERROR_CHECK(gpio_isr_handler_add(Config::STOP_PIN, stop_isr, nullptr));

  ESP_LOGI("conveyor", "part sensor GPIO %d, emergency stop BOOT, alarm LED GPIO %d",
           Config::PART_PIN, Config::LED_PIN);
}
