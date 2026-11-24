// ДЗ 6.3 на STM32F411 (Black Pill): кілька датчиків -> одна черга -> один consumer.
//
//   temp   (задача) - вбудований датчик температури (ADC1, канал 18), кожні 1000 мс;
//   pot    (задача) - потенціометр на PA1 (ADC1, канал 1), середнє з 16 вимірювань, кожні 250 мс;
//   button (ISR)    - кнопка KEY (PA0) через EXTI0, подія на кожне натискання;
//   consumer        - єдиний читач черги і єдиний власник статистики.
//
// Дані між задачами йдуть тільки через чергу. Два м'ютекси захищають периферію, а не дані:
// ADC1 спільний для temp і pot, USART1 - для всіх, хто пише в лог.

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>

#include "stm32f4xx_hal.h"
#include "stm32f4xx_ll_adc.h"  // адреси калібрувань VREFINT_CAL і TS_CAL у системній пам'яті

#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"

struct Config {
  static constexpr uint32_t POT_CHANNEL = ADC_CHANNEL_1;  // PA1
  static constexpr int      POT_SAMPLES = 16;
  static constexpr int      REF_SAMPLES = 4;

  static constexpr uint32_t BUTTON_QUIET_MS = 30;  // тиша на піні перед натисканням
  static constexpr uint32_t BUTTON_IRQ_PRIO = 6;   // не вище за configLIBRARY_MAX_SYSCALL (5)

  static constexpr uint32_t TEMP_PERIOD_MS  = 1000;
  static constexpr uint32_t POT_PERIOD_MS   = 250;
  static constexpr uint32_t SEND_TIMEOUT_MS = 10;
  static constexpr uint32_t RECV_TIMEOUT_MS = 500;
  static constexpr uint32_t SILENCE_PERIODS = 3;
  static constexpr uint32_t STATS_PERIOD_MS = 10000;

  // Експеримент «повільний consumer»: 0 - звичайна робота, 500 - черга переповниться.
  static constexpr uint32_t CONSUMER_DELAY_MS = 0;

  static constexpr UBaseType_t QUEUE_LENGTH = 8;

  static constexpr UBaseType_t PRIO_PRODUCER = 2;
  static constexpr UBaseType_t PRIO_CONSUMER = 3;
  static constexpr configSTACK_DEPTH_TYPE STACK_WORDS = 256;  // у словах: 1 КБ

  static constexpr uint32_t LOG_BAUD = 115200;
};

static_assert(Config::SEND_TIMEOUT_MS < Config::POT_PERIOD_MS, "send timeout must fit in a period");
static_assert(Config::RECV_TIMEOUT_MS < Config::SILENCE_PERIODS * Config::POT_PERIOD_MS,
              "consumer must check silence more often than the shortest window");
static_assert(Config::PRIO_PRODUCER < Config::PRIO_CONSUMER, "consumer must outrank producers");
static_assert(Config::BUTTON_IRQ_PRIO >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY,
              "ISR that calls FromISR API must not outrank configMAX_SYSCALL_INTERRUPT_PRIORITY");

#define LOG_PORT    GPIOA
#define LOG_TX_PIN  GPIO_PIN_9   // USART1_TX, AF7
#define POT_PORT    GPIOA
#define POT_PIN     GPIO_PIN_1
#define BUTTON_PORT GPIOA
#define BUTTON_PIN  GPIO_PIN_0   // KEY на платі, натиснута = 0
#define LED_PORT    GPIOC
#define LED_PIN     GPIO_PIN_13  // світлодіод плати, горить від низького рівня

static UART_HandleTypeDef huart1;
static ADC_HandleTypeDef  hadc1;
static SemaphoreHandle_t  uart_mutex;
static SemaphoreHandle_t  adc_mutex;

static void Error_Handler(void);

// ---------------------------------------------------------------------------
// Лог: тік береться до м'ютекса, тому в лозі момент події, а не момент, коли звільнився UART.
// ---------------------------------------------------------------------------
static uint32_t now_ms(void)
{
  return xTaskGetTickCount() * portTICK_PERIOD_MS;
}

static void log_line(const char *fmt, ...)
{
  char line[80];
  int n = snprintf(line, sizeof line, "%7" PRIu32 "  ", now_ms());

  va_list args;
  va_start(args, fmt);
  n += vsnprintf(line + n, sizeof line - n, fmt, args);
  va_end(args);

  if (n > (int)sizeof line - 3) {
    n = sizeof line - 3;
  }
  line[n++] = '\r';
  line[n++] = '\n';

  xSemaphoreTake(uart_mutex, portMAX_DELAY);
  HAL_UART_Transmit(&huart1, (uint8_t *)line, n, HAL_MAX_DELAY);
  xSemaphoreGive(uart_mutex);
}

// ---------------------------------------------------------------------------
// Повідомлення. Черга копіює всі 16 байт, тому відправник одразу може перевикористати змінну.
// ---------------------------------------------------------------------------
enum class Source : uint8_t { Temp, Pot, Button, Count };
static constexpr size_t SOURCE_COUNT = static_cast<size_t>(Source::Count);

struct Message {
  Source   source;   // від кого
  uint32_t seq;      // номер від цього джерела: розрив = втрачені повідомлення
  uint32_t time_ms;  // момент вимірювання або натискання
  int32_t  value;    // temp: соті частки °C; pot: мВ; button: 0
};
static_assert(sizeof(Message) == 16, "message is copied into the queue, keep it small");

static QueueHandle_t queue;  // створюється першим, до датчиків, задач і переривання

// ---------------------------------------------------------------------------
// Датчики. Обидва читають ADC1, тому кожне читання - під adc_mutex.
// ---------------------------------------------------------------------------
static bool adc_average(uint32_t channel, int samples, uint32_t *out)
{
  ADC_ChannelConfTypeDef ch = {};
  ch.Channel      = channel;
  ch.Rank         = 1;
  ch.SamplingTime = ADC_SAMPLETIME_480CYCLES;  // датчику температури треба не менше 10 мкс
  if (HAL_ADC_ConfigChannel(&hadc1, &ch) != HAL_OK) {
    return false;
  }

  uint32_t sum = 0;
  for (int i = 0; i < samples; ++i) {
    HAL_ADC_Start(&hadc1);
    if (HAL_ADC_PollForConversion(&hadc1, 10) != HAL_OK) {
      HAL_ADC_Stop(&hadc1);
      return false;
    }
    sum += HAL_ADC_GetValue(&hadc1);
    HAL_ADC_Stop(&hadc1);
  }
  *out = sum / samples;
  return true;
}

// Код АЦП каналу разом з напругою живлення АЦП (VDDA). VDDA береться з VREFINT і калібрування
// VREFINT_CAL: живлення від USB рідко дає рівно 3,3 В.
static bool adc_read(uint32_t channel, int samples, uint32_t *raw, uint32_t *vdda_mv)
{
  uint32_t vref = 0;
  xSemaphoreTake(adc_mutex, portMAX_DELAY);
  bool ok = adc_average(ADC_CHANNEL_VREFINT, Config::REF_SAMPLES, &vref) &&
            adc_average(channel, samples, raw);
  xSemaphoreGive(adc_mutex);

  if (!ok || vref == 0) {
    return false;
  }
  *vdda_mv = VREFINT_CAL_VREF * *VREFINT_CAL_ADDR / vref;
  return true;
}

static bool read_temp(int32_t *out)
{
  uint32_t raw = 0;
  uint32_t vdda = 0;
  if (!adc_read(ADC_CHANNEL_TEMPSENSOR, 1, &raw, &vdda)) {
    return false;
  }
  // TS_CAL1 і TS_CAL2 виміряні на заводі при 30 і 110 °C і VDDA = 3,3 В.
  int32_t raw_3v3 = (int32_t)(raw * vdda / TEMPSENSOR_CAL_VREFANALOG);
  int32_t cal1 = *TEMPSENSOR_CAL1_ADDR;
  int32_t cal2 = *TEMPSENSOR_CAL2_ADDR;
  *out = (TEMPSENSOR_CAL2_TEMP - TEMPSENSOR_CAL1_TEMP) * 100 * (raw_3v3 - cal1) / (cal2 - cal1) +
         TEMPSENSOR_CAL1_TEMP * 100;
  return true;
}

static bool read_pot(int32_t *out)
{
  uint32_t raw = 0;
  uint32_t vdda = 0;
  if (!adc_read(Config::POT_CHANNEL, Config::POT_SAMPLES, &raw, &vdda)) {
    return false;
  }
  *out = (int32_t)(raw * vdda / 4095);
  return true;
}

// ---------------------------------------------------------------------------
// Продюсери: одна функція на всі датчики, параметр - рядок таблиці.
// ---------------------------------------------------------------------------
struct Sensor {
  Source      source;
  const char *name;       // і ім'я задачі
  uint32_t    period_ms;  // 0 - джерело без періоду (кнопка)
  bool (*read)(int32_t *out);
};

// Індекс у таблиці = номер джерела, тому SENSORS[msg.source] завжди знаходить своє джерело.
static const Sensor SENSORS[SOURCE_COUNT] = {
  {Source::Temp,   "temp",   Config::TEMP_PERIOD_MS, read_temp},
  {Source::Pot,    "pot",    Config::POT_PERIOD_MS,  read_pot},
  {Source::Button, "button", 0,                      nullptr},
};

static void producer_task(void *arg)
{
  const Sensor *sensor = static_cast<const Sensor *>(arg);
  const TickType_t period = pdMS_TO_TICKS(sensor->period_ms);
  TickType_t last_wake = xTaskGetTickCount();
  uint32_t seq = 0;

  for (;;) {
    vTaskDelayUntil(&last_wake, period);

    int32_t value = 0;
    if (!sensor->read(&value)) {
      log_line("%-6s read error", sensor->name);
      continue;
    }

    Message msg = {};
    msg.source  = sensor->source;
    msg.seq     = seq++;  // росте і при невдалій відправці: consumer побачить розрив
    msg.time_ms = last_wake * portTICK_PERIOD_MS;
    msg.value   = value;

    // Обмежений таймаут: якщо consumer завис, датчик не зупиняється, а втрата видна в лозі.
    if (xQueueSend(queue, &msg, pdMS_TO_TICKS(Config::SEND_TIMEOUT_MS)) != pdPASS) {
      log_line("%-6s queue full, #%" PRIu32 " dropped", sensor->name, msg.seq);
    }
  }
}

// ---------------------------------------------------------------------------
// ISR кнопки. Тільки ...FromISR API, без логу. Змінні static чіпає лише цей ISR.
// ---------------------------------------------------------------------------

// Єдина змінна поза чергою: ISR не може надрукувати «queue full», тому рахує втрати тут.
// Пише тільки ISR, читає тільки consumer. Запис і читання 32 біт на Cortex-M4 атомарні.
static volatile uint32_t button_dropped;

// Рівень піна на останньому фронті. button_init читає початкове значення з піна.
static GPIO_PinState button_level;

static void button_isr(void)
{
  static TickType_t last_edge;
  static uint32_t   seq;

  // Переривання на обидва фронти. Брязкіт іде пачкою фронтів без тиші між ними.
  // Натискання - перший фронт після тиші, якщо до тиші кнопка була відпущена.
  // Рівень у момент ISR не годиться: брязкіт міг уже змінити його знову. Стабільний рівень
  // до тиші - це рівень, який ISR прочитав на останньому фронті попередньої пачки.
  TickType_t now = xTaskGetTickCountFromISR();
  bool quiet = now - last_edge >= pdMS_TO_TICKS(Config::BUTTON_QUIET_MS);
  bool was_released = button_level == GPIO_PIN_SET;
  last_edge    = now;
  button_level = HAL_GPIO_ReadPin(BUTTON_PORT, BUTTON_PIN);
  if (!quiet || !was_released) {
    return;
  }

  Message msg = {};
  msg.source  = Source::Button;
  msg.seq     = seq++;
  msg.time_ms = now * portTICK_PERIOD_MS;

  // ISR не може чекати: якщо черга повна, подія губиться. Розрив у seq consumer побачить
  // лише на наступному натисканні, тому ISR ще й рахує втрату в button_dropped.
  BaseType_t woken = pdFALSE;
  if (xQueueSendFromISR(queue, &msg, &woken) != pdPASS) {
    button_dropped = button_dropped + 1;
  }
  // Якщо ISR розбудив задачу з вищим пріоритетом, перемкнутися одразу після виходу з ISR.
  portYIELD_FROM_ISR(woken);
}

extern "C" void EXTI0_IRQHandler(void)
{
  __HAL_GPIO_EXTI_CLEAR_IT(BUTTON_PIN);  // скинути до читання піна: новий фронт викличе ISR знову
  button_isr();
}

static void button_init(void)
{
  GPIO_InitTypeDef gpio = {};
  gpio.Pin  = BUTTON_PIN;
  gpio.Mode = GPIO_MODE_IT_RISING_FALLING;
  gpio.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(BUTTON_PORT, &gpio);
  button_level = HAL_GPIO_ReadPin(BUTTON_PORT, BUTTON_PIN);

  HAL_NVIC_SetPriority(EXTI0_IRQn, Config::BUTTON_IRQ_PRIO, 0);
  HAL_NVIC_EnableIRQ(EXTI0_IRQn);
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
      int32_t a = v < 0 ? -v : v;
      log_line("%-6s #%-4" PRIu32 " %s%" PRId32 ".%02" PRId32 " C   wait %" PRIu32 " ms", name,
               msg.seq, v < 0 ? "-" : "", a / 100, a % 100, wait);
      break;
    }
    case Source::Pot:
      log_line("%-6s #%-4" PRIu32 " %" PRId32 " mV   wait %" PRIu32 " ms", name, msg.seq,
               msg.value, wait);
      break;
    case Source::Button:
      log_line("%-6s #%-4" PRIu32 " pressed   wait %" PRIu32 " ms", name, msg.seq, wait);
      break;
    default:
      log_line("%-6s no handler", name);
      break;
  }
}

static void handle_message(Stats stats[], const Message &msg)
{
  // Повідомлення прийшло ззовні задачі: перевірити межі до звертання до масиву.
  size_t index = static_cast<size_t>(msg.source);
  if (index >= SOURCE_COUNT) {
    log_line("unknown source %u, message dropped", unsigned(index));
    return;
  }
  Stats &st = stats[index];
  const char *name = SENSORS[index].name;

  // Черга - FIFO: від одного джерела номери йдуть підряд. Розрив = загублені повідомлення.
  if (msg.seq > st.next_seq) {
    uint32_t gap = msg.seq - st.next_seq;
    st.lost += gap;
    log_line("%-6s lost %" PRIu32 " before #%" PRIu32, name, gap, msg.seq);
  }
  st.next_seq = msg.seq + 1;
  st.received++;
  st.last_rx = xTaskGetTickCount();
  if (st.silent) {
    st.silent = false;
    log_line("%-6s back online", name);
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
      log_line("%-6s no data for %" PRIu32 " ms", SENSORS[i].name, window_ms);
    }
  }
}

static void print_stats(const Stats stats[], UBaseType_t peak)
{
  for (size_t i = 0; i < SOURCE_COUNT; ++i) {
    log_line("stats  %-6s received %" PRIu32 ", lost %" PRIu32, SENSORS[i].name,
             stats[i].received, stats[i].lost);
  }
  log_line("stats  queue  peak %u of %u, ISR dropped %" PRIu32, unsigned(peak),
           unsigned(Config::QUEUE_LENGTH), button_dropped);
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
      // Разом із щойно взятим. ISR міг уже дописати повідомлення, тому обмеження зверху.
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
// Старт: черга -> периферія -> consumer -> продюсери -> переривання -> планувальник.
// ---------------------------------------------------------------------------
static void start(TaskFunction_t fn, const char *name, void *arg, UBaseType_t prio)
{
  if (xTaskCreate(fn, name, Config::STACK_WORDS, arg, prio, nullptr) != pdPASS) {
    Error_Handler();
  }
}

static void SystemClock_Config(void);
static void led_init(void);
static void uart_init(void);
static void adc_init(void);

int main(void)
{
  HAL_Init();
  SystemClock_Config();
  led_init();
  uart_init();

  queue      = xQueueCreate(Config::QUEUE_LENGTH, sizeof(Message));
  uart_mutex = xSemaphoreCreateMutex();
  adc_mutex  = xSemaphoreCreateMutex();
  if (queue == nullptr || uart_mutex == nullptr || adc_mutex == nullptr) {
    Error_Handler();
  }

  adc_init();

  // Планувальник ще не працює, тому рядок іде в UART напряму, без м'ютекса.
  static const char banner[] =
      "\r\n=== dz6.3 sensor_queue_stm32: temp, pot, KEY -> queue of 8 -> consumer ===\r\n";
  HAL_UART_Transmit(&huart1, (uint8_t *)banner, sizeof banner - 1, HAL_MAX_DELAY);

  start(consumer_task, "consumer", nullptr, Config::PRIO_CONSUMER);
  for (const Sensor &sensor : SENSORS) {
    if (sensor.period_ms > 0) {
      start(producer_task, sensor.name, const_cast<Sensor *>(&sensor), Config::PRIO_PRODUCER);
    }
  }

  // xTaskCreate маскує переривання до старту планувальника, тому EXTI0 не спрацює раніше.
  button_init();

  vTaskStartScheduler();

  Error_Handler();  // сюди потрапляємо, лише якщо не вистачило купи на idle-задачу
}

// SysTick потрібен обом: HAL рахує в ньому HAL_GetTick() (на ньому тримаються
// тайм-аути HAL_RCC, HAL_UART і HAL_ADC), а FreeRTOS - свій тік.
extern "C" void xPortSysTickHandler(void);

extern "C" void SysTick_Handler(void)
{
  HAL_IncTick();
  if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
    xPortSysTickHandler();
  }
}

static void SystemClock_Config(void)
{
  // HSI 16 МГц без PLL, як у ДЗ 2.8, 2.9, 6.1 і 6.2.
  RCC_OscInitTypeDef osc = {};
  osc.OscillatorType      = RCC_OSCILLATORTYPE_HSI;
  osc.HSIState            = RCC_HSI_ON;
  osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  osc.PLL.PLLState        = RCC_PLL_NONE;
  if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
    Error_Handler();
  }

  RCC_ClkInitTypeDef clk = {};
  clk.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_PCLK1 |
                  RCC_CLOCKTYPE_PCLK2;
  clk.SYSCLKSource   = RCC_SYSCLKSOURCE_HSI;
  clk.AHBCLKDivider  = RCC_SYSCLK_DIV1;
  clk.APB1CLKDivider = RCC_HCLK_DIV1;
  clk.APB2CLKDivider = RCC_HCLK_DIV1;
  if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_0) != HAL_OK) {
    Error_Handler();
  }
}

static void led_init(void)
{
  __HAL_RCC_GPIOC_CLK_ENABLE();

  GPIO_InitTypeDef gpio = {};
  gpio.Pin   = LED_PIN;
  gpio.Mode  = GPIO_MODE_OUTPUT_PP;
  gpio.Pull  = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_SET);  // вимкнений
  HAL_GPIO_Init(LED_PORT, &gpio);
}

static void uart_init(void)
{
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_USART1_CLK_ENABLE();

  GPIO_InitTypeDef tx = {};
  tx.Pin       = LOG_TX_PIN;
  tx.Mode      = GPIO_MODE_AF_PP;
  tx.Pull      = GPIO_NOPULL;
  tx.Speed     = GPIO_SPEED_FREQ_HIGH;
  tx.Alternate = GPIO_AF7_USART1;
  HAL_GPIO_Init(LOG_PORT, &tx);

  huart1.Instance          = USART1;
  huart1.Init.BaudRate     = Config::LOG_BAUD;
  huart1.Init.WordLength   = UART_WORDLENGTH_8B;
  huart1.Init.StopBits     = UART_STOPBITS_1;
  huart1.Init.Parity       = UART_PARITY_NONE;
  huart1.Init.Mode         = UART_MODE_TX;
  huart1.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK) {
    Error_Handler();
  }
}

static void adc_init(void)
{
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_ADC1_CLK_ENABLE();

  GPIO_InitTypeDef pot = {};
  pot.Pin  = POT_PIN;
  pot.Mode = GPIO_MODE_ANALOG;
  pot.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(POT_PORT, &pot);

  // ADCCLK = PCLK2 / 2 = 8 МГц, у межах 36 МГц. 480 тактів вибірки = 60 мкс.
  hadc1.Instance                   = ADC1;
  hadc1.Init.ClockPrescaler        = ADC_CLOCK_SYNC_PCLK_DIV2;
  hadc1.Init.Resolution            = ADC_RESOLUTION_12B;
  hadc1.Init.DataAlign             = ADC_DATAALIGN_RIGHT;
  hadc1.Init.ScanConvMode          = DISABLE;
  hadc1.Init.ContinuousConvMode    = DISABLE;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConv      = ADC_SOFTWARE_START;
  hadc1.Init.ExternalTrigConvEdge  = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc1.Init.NbrOfConversion       = 1;
  hadc1.Init.DMAContinuousRequests = DISABLE;
  hadc1.Init.EOCSelection          = ADC_EOC_SINGLE_CONV;
  if (HAL_ADC_Init(&hadc1) != HAL_OK) {
    Error_Handler();
  }
}

// Будь-яка фатальна помилка: світлодіод плати горить постійно.
static void Error_Handler(void)
{
  __disable_irq();
  LED_PORT->BSRR = (uint32_t)LED_PIN << 16;  // скинути PC13 = увімкнути
  while (1) {
  }
}

extern "C" void rtos_assert_failed(void) { Error_Handler(); }
extern "C" void vApplicationMallocFailedHook(void) { Error_Handler(); }
extern "C" void vApplicationStackOverflowHook(TaskHandle_t, char *) { Error_Handler(); }
