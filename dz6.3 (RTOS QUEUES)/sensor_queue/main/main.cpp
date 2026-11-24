// ДЗ 6.3: кілька датчиків -> одна черга -> один consumer.
//
//   temp   (задача, ядро 0) - вбудований датчик температури, кожні 1000 мс;
//   pot    (задача, ядро 0) - потенціометр на GPIO1, середнє з 16 вимірювань, кожні 250 мс;
//   button (ISR)            - кнопка BOOT (GPIO0), подія на кожне натискання;
//   consumer (задача, ядро 1) - єдиний читач черги і єдиний власник статистики.
//
// Дані між задачами йдуть тільки через чергу: кожне джерело кладе в неї КОПІЮ повідомлення.

#include <inttypes.h>
#include <math.h>
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

static const char *TAG = "queue";

struct Config {
  // Потенціометр: крайні виводи на 3V3 і GND, повзунок на GPIO1 = ADC1_CH0.
  static constexpr adc_unit_t    POT_UNIT    = ADC_UNIT_1;
  static constexpr adc_channel_t POT_CHANNEL = ADC_CHANNEL_0;
  static constexpr adc_atten_t   POT_ATTEN   = ADC_ATTEN_DB_12;  // діапазон до ~3,1 В
  static constexpr int           POT_SAMPLES = 16;

  static constexpr gpio_num_t BUTTON_PIN      = GPIO_NUM_0;  // BOOT, натиснута = 0
  static constexpr uint32_t   BUTTON_QUIET_MS = 30;          // тиша на піні перед натисканням

  static constexpr uint32_t TEMP_PERIOD_MS  = 1000;
  static constexpr uint32_t POT_PERIOD_MS   = 250;
  static constexpr uint32_t SEND_TIMEOUT_MS = 10;     // продюсер чекає місця в черзі
  static constexpr uint32_t RECV_TIMEOUT_MS = 500;    // consumer чекає повідомлення
  static constexpr uint32_t SILENCE_PERIODS = 3;      // стільки періодів без даних = датчик зник
  static constexpr uint32_t STATS_PERIOD_MS = 10000;

  // Експеримент «повільний consumer»: 0 - звичайна робота, 500 - черга переповниться.
  static constexpr uint32_t CONSUMER_DELAY_MS = 0;

  static constexpr UBaseType_t QUEUE_LENGTH = 8;

  static constexpr UBaseType_t PRIO_PRODUCER = 2;
  static constexpr UBaseType_t PRIO_CONSUMER = 3;
  static constexpr BaseType_t  CORE_PRODUCER = 0;
  static constexpr BaseType_t  CORE_CONSUMER = 1;
  static constexpr uint32_t    STACK         = 3072;
};

static_assert(Config::SEND_TIMEOUT_MS < Config::POT_PERIOD_MS, "send timeout must fit in a period");
static_assert(Config::RECV_TIMEOUT_MS < Config::SILENCE_PERIODS * Config::POT_PERIOD_MS,
              "consumer must check silence more often than the shortest window");
static_assert(Config::PRIO_PRODUCER < Config::PRIO_CONSUMER, "consumer must outrank producers");

// ---------------------------------------------------------------------------
// Повідомлення. Черга копіює всі 16 байт, тому відправник одразу може перевикористати змінну.
// ---------------------------------------------------------------------------
enum class Source : uint8_t { Temp, Pot, Button, Count };
static constexpr size_t SOURCE_COUNT = static_cast<size_t>(Source::Count);

struct Message {
  Source   source;   // від кого
  uint8_t  core;     // ядро, на якому працював відправник
  uint32_t seq;      // номер від цього джерела: розрив = втрачені повідомлення
  uint32_t time_ms;  // момент вимірювання або натискання
  int32_t  value;    // temp: соті частки °C; pot: мВ; button: 0
};
static_assert(sizeof(Message) == 16, "message is copied into the queue, keep it small");

static QueueHandle_t queue;  // створюється першим, до датчиків, задач і ISR

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
    *out = static_cast<int32_t>(lroundf(celsius * 100));
  }
  return err;
}

static esp_err_t read_pot(int32_t *out)
{
  int sum = 0;
  for (int i = 0; i < Config::POT_SAMPLES; ++i) {
    int raw = 0;
    esp_err_t err = adc_oneshot_read(adc, Config::POT_CHANNEL, &raw);
    if (err != ESP_OK) {
      return err;
    }
    sum += raw;
  }
  // Калібрування з eFuse переводить код АЦП у мВ з урахуванням нелінійності.
  int mv = 0;
  esp_err_t err = adc_cali_raw_to_voltage(adc_cali, sum / Config::POT_SAMPLES, &mv);
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

// ---------------------------------------------------------------------------
// Продюсери: одна функція на всі датчики, параметр - рядок таблиці.
// ---------------------------------------------------------------------------
struct Sensor {
  Source      source;
  const char *name;       // і ім'я задачі (до 15 символів)
  uint32_t    period_ms;  // 0 - джерело без періоду (кнопка)
  esp_err_t (*read)(int32_t *out);
};

// Індекс у таблиці = номер джерела, тому SENSORS[msg.source] завжди знаходить своє джерело.
static const Sensor SENSORS[SOURCE_COUNT] = {
  {Source::Temp,   "temp",   Config::TEMP_PERIOD_MS, read_temp},
  {Source::Pot,    "pot",    Config::POT_PERIOD_MS,  read_pot},
  {Source::Button, "button", 0,                      nullptr},
};

static uint32_t now_ms(void)
{
  return pdTICKS_TO_MS(xTaskGetTickCount());
}

static void producer_task(void *arg)
{
  const Sensor *sensor = static_cast<const Sensor *>(arg);
  const TickType_t period = pdMS_TO_TICKS(sensor->period_ms);
  TickType_t last_wake = xTaskGetTickCount();
  uint32_t seq = 0;

  for (;;) {
    vTaskDelayUntil(&last_wake, period);

    int32_t value = 0;
    esp_err_t err = sensor->read(&value);
    if (err != ESP_OK) {
      printf("%7" PRIu32 "  %-6s read error: %s\n", now_ms(), sensor->name, esp_err_to_name(err));
      continue;
    }

    Message msg = {};
    msg.source  = sensor->source;
    msg.core    = static_cast<uint8_t>(xPortGetCoreID());
    msg.seq     = seq++;  // росте і при невдалій відправці: consumer побачить розрив
    msg.time_ms = pdTICKS_TO_MS(last_wake);
    msg.value   = value;

    // Обмежений таймаут: якщо consumer завис, датчик не зупиняється, а втрата видна в лозі.
    if (xQueueSend(queue, &msg, pdMS_TO_TICKS(Config::SEND_TIMEOUT_MS)) != pdPASS) {
      printf("%7" PRIu32 "  %-6s queue full, #%" PRIu32 " dropped\n", now_ms(), sensor->name, msg.seq);
    }
  }
}

// ---------------------------------------------------------------------------
// ISR кнопки. Тільки ...FromISR API, без printf. Змінні static чіпає лише цей ISR.
// ---------------------------------------------------------------------------

// Єдина змінна поза чергою: ISR не може надрукувати «queue full», тому рахує втрати тут.
// Пише тільки ISR, читає тільки consumer. Запис і читання 32 біт на ESP32-S3 атомарні.
static volatile uint32_t button_dropped;

// Рівень піна на останньому фронті. button_init читає початкове значення з піна.
static int button_level;

// Без IRAM_ATTR: сервіс GPIO встановлено без ESP_INTR_FLAG_IRAM, тому на час запису у flash
// переривання чекає, а gpio_get_level з flash безпечний.
static void button_isr(void *)
{
  static TickType_t last_edge;
  static uint32_t   seq;

  // Переривання на обидва фронти. Брязкіт іде пачкою фронтів без тиші між ними.
  // Натискання - перший фронт після тиші, якщо до тиші кнопка була відпущена.
  // Рівень у момент ISR не годиться: брязкіт міг уже змінити його знову. Стабільний рівень
  // до тиші - це рівень, який ISR прочитав на останньому фронті попередньої пачки.
  TickType_t now = xTaskGetTickCountFromISR();
  bool quiet = now - last_edge >= pdMS_TO_TICKS(Config::BUTTON_QUIET_MS);
  bool was_released = button_level != 0;
  last_edge    = now;
  button_level = gpio_get_level(Config::BUTTON_PIN);
  if (!quiet || !was_released) {
    return;
  }

  Message msg = {};
  msg.source  = Source::Button;
  msg.core    = static_cast<uint8_t>(xPortGetCoreID());
  msg.seq     = seq++;
  msg.time_ms = pdTICKS_TO_MS(now);

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
  gpio_config_t cfg = {};
  cfg.pin_bit_mask = 1ULL << Config::BUTTON_PIN;
  cfg.mode         = GPIO_MODE_INPUT;
  cfg.pull_up_en   = GPIO_PULLUP_ENABLE;
  cfg.intr_type    = GPIO_INTR_ANYEDGE;
  ESP_ERROR_CHECK(gpio_config(&cfg));
  button_level = gpio_get_level(Config::BUTTON_PIN);
  ESP_ERROR_CHECK(gpio_install_isr_service(0));
  ESP_ERROR_CHECK(gpio_isr_handler_add(Config::BUTTON_PIN, button_isr, nullptr));
}

// ---------------------------------------------------------------------------
// Consumer
// ---------------------------------------------------------------------------
struct Stats {
  uint32_t   received;
  uint32_t   lost;
  uint32_t   next_seq;
  TickType_t last_rx;
  bool       silent;
};

static void print_message(const Message &msg, const char *name)
{
  uint32_t wait = now_ms() - msg.time_ms;

  switch (msg.source) {
    case Source::Temp: {
      int32_t v = msg.value;
      printf("%7" PRIu32 "  %-6s #%-4" PRIu32 " %s%" PRId32 ".%02" PRId32 " C   core %u, wait %" PRIu32 " ms\n",
             now_ms(), name, msg.seq, v < 0 ? "-" : "", labs(v) / 100, labs(v) % 100, msg.core, wait);
      break;
    }
    case Source::Pot:
      printf("%7" PRIu32 "  %-6s #%-4" PRIu32 " %" PRId32 " mV   core %u, wait %" PRIu32 " ms\n",
             now_ms(), name, msg.seq, msg.value, msg.core, wait);
      break;
    case Source::Button:
      printf("%7" PRIu32 "  %-6s #%-4" PRIu32 " pressed   core %u, wait %" PRIu32 " ms\n",
             now_ms(), name, msg.seq, msg.core, wait);
      break;
    default:
      printf("%7" PRIu32 "  %-6s no handler\n", now_ms(), name);
      break;
  }
}

static void handle_message(Stats stats[], const Message &msg)
{
  // Повідомлення прийшло ззовні задачі: перевірити межі до звертання до масиву.
  size_t index = static_cast<size_t>(msg.source);
  if (index >= SOURCE_COUNT) {
    printf("%7" PRIu32 "  unknown source %u, message dropped\n", now_ms(), unsigned(index));
    return;
  }
  Stats &st = stats[index];
  const char *name = SENSORS[index].name;

  // Черга - FIFO: від одного джерела номери йдуть підряд. Розрив = загублені повідомлення.
  if (msg.seq > st.next_seq) {
    uint32_t gap = msg.seq - st.next_seq;
    st.lost += gap;
    printf("%7" PRIu32 "  %-6s lost %" PRIu32 " before #%" PRIu32 "\n", now_ms(), name, gap, msg.seq);
  }
  st.next_seq = msg.seq + 1;
  st.received++;
  st.last_rx = xTaskGetTickCount();
  if (st.silent) {
    st.silent = false;
    printf("%7" PRIu32 "  %-6s back online\n", now_ms(), name);
  }

  print_message(msg, name);
}

// Тиша перевіряється для кожного датчика окремо. Якщо замовк один датчик, а інший працює,
// загальний таймаут xQueueReceive не спрацює: повідомлення в черзі є.
static void check_silence(Stats stats[])
{
  TickType_t now = xTaskGetTickCount();
  for (size_t i = 0; i < SOURCE_COUNT; ++i) {
    if (SENSORS[i].period_ms == 0 || stats[i].silent) {
      continue;
    }
    uint32_t window_ms = Config::SILENCE_PERIODS * SENSORS[i].period_ms;
    if (now - stats[i].last_rx >= pdMS_TO_TICKS(window_ms)) {
      stats[i].silent = true;
      printf("%7" PRIu32 "  %-6s no data for %" PRIu32 " ms\n", now_ms(), SENSORS[i].name, window_ms);
    }
  }
}

static void print_stats(const Stats stats[], UBaseType_t peak)
{
  for (size_t i = 0; i < SOURCE_COUNT; ++i) {
    printf("%7" PRIu32 "  stats  %-6s received %" PRIu32 ", lost %" PRIu32 "\n", now_ms(),
           SENSORS[i].name, stats[i].received, stats[i].lost);
  }
  printf("%7" PRIu32 "  stats  queue  peak %u of %u, ISR dropped %" PRIu32 "\n", now_ms(),
         unsigned(peak), unsigned(Config::QUEUE_LENGTH), button_dropped);
}

static void consumer_task(void *)
{
  // Статистику бачить тільки ця задача, тому гонки немає без жодного mutex.
  Stats stats[SOURCE_COUNT] = {};
  TickType_t start = xTaskGetTickCount();
  for (Stats &st : stats) {
    st.last_rx = start;
  }
  TickType_t last_stats = start;
  UBaseType_t peak = 0;

  for (;;) {
    Message msg;
    if (xQueueReceive(queue, &msg, pdMS_TO_TICKS(Config::RECV_TIMEOUT_MS)) == pdPASS) {
      // Разом із щойно взятим. Продюсер на ядрі 0 міг уже дописати повідомлення, тому
      // без обмеження вийшло б «9 з 8».
      UBaseType_t depth = uxQueueMessagesWaiting(queue) + 1;
      if (depth > Config::QUEUE_LENGTH) {
        depth = Config::QUEUE_LENGTH;
      }
      if (depth > peak) {
        peak = depth;
      }
      handle_message(stats, msg);
      if (Config::CONSUMER_DELAY_MS > 0) {
        vTaskDelay(pdMS_TO_TICKS(Config::CONSUMER_DELAY_MS));
      }
    }

    check_silence(stats);

    if (xTaskGetTickCount() - last_stats >= pdMS_TO_TICKS(Config::STATS_PERIOD_MS)) {
      last_stats += pdMS_TO_TICKS(Config::STATS_PERIOD_MS);
      print_stats(stats, peak);
    }
  }
}

// ---------------------------------------------------------------------------
// Старт: черга -> датчики -> consumer -> продюсери -> переривання.
// Кожен крок спирається на попередній: продюсер або ISR до створення черги відправив би в NULL.
// ---------------------------------------------------------------------------
static void start(TaskFunction_t fn, const char *name, void *arg, UBaseType_t prio, BaseType_t core)
{
  if (xTaskCreatePinnedToCore(fn, name, Config::STACK, arg, prio, nullptr, core) != pdPASS) {
    ESP_LOGE(TAG, "no memory for task %s", name);
    abort();
  }
}

extern "C" void app_main(void)
{
  queue = xQueueCreate(Config::QUEUE_LENGTH, sizeof(Message));
  if (queue == nullptr) {
    ESP_LOGE(TAG, "no memory for queue");
    abort();
  }

  sensors_init();

  start(consumer_task, "consumer", nullptr, Config::PRIO_CONSUMER, Config::CORE_CONSUMER);

  for (const Sensor &sensor : SENSORS) {
    if (sensor.period_ms > 0) {
      start(producer_task, sensor.name, const_cast<Sensor *>(&sensor),
            Config::PRIO_PRODUCER, Config::CORE_PRODUCER);
    }
  }

  button_init();

  ESP_LOGI(TAG, "temp, pot on core %d -> queue of %u -> consumer on core %d, BOOT button via ISR",
           int(Config::CORE_PRODUCER), unsigned(Config::QUEUE_LENGTH), int(Config::CORE_CONSUMER));
}
