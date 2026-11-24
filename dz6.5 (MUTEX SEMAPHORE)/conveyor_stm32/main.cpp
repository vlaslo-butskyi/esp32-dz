// ДЗ 6.5 на STM32F411 (Black Pill): конвеєр, спільні ресурси, ISR і аварійна зупинка.
//
//   part_isr (EXTI15_10, кнопка PB12) - датчик деталі: лічильник parts += 1;
//   stop_isr (EXTI0, кнопка KEY PA0)  - аварійна зупинка: будить задачу estop;
//   report (Task 1) - раз на секунду: лічильник -> швидкість, деталі/хв -> буфер звіту;
//   adc    (Task 2) - після кожного звіту Task 1: АЦП (PA1) -> дописати в той самий буфер;
//   estop  (Task 3, найвищий пріоритет) - спить до stop_isr, потім зупиняє конвеєр.
//
// Логіка та сама, що в ESP32-версії conveyor. Ядро одне, тому:
//   - спінлок замінює taskENTER_CRITICAL: він маскує переривання до configMAX_SYSCALL;
//   - задачі не прив'язані до ядер, report і adc працюють по черзі на одному ядрі.
// Лог іде в USART1 (PA9). UART має свій mutex. Задача ніколи не тримає два mutex одночасно.

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "stm32f4xx_hal.h"
#include "stm32f4xx_ll_adc.h"  // адреса калібрування VREFINT_CAL у системній пам'яті

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

struct Config {
  static constexpr uint16_t PART_PIN   = GPIO_PIN_12;  // PB12, кнопка на GND, натиснута = 0
  static constexpr uint16_t STOP_PIN   = GPIO_PIN_0;   // KEY на платі, натиснута = 0
  static constexpr uint16_t LED_PIN    = GPIO_PIN_13;  // світлодіод плати, горить від 0
  static constexpr uint16_t POT_PIN    = GPIO_PIN_1;   // PA1 = ADC1, канал 1
  static constexpr uint16_t LOG_TX_PIN = GPIO_PIN_9;   // USART1_TX, AF7

  static constexpr uint32_t POT_CHANNEL = ADC_CHANNEL_1;
  static constexpr int      POT_SAMPLES = 16;
  static constexpr int      REF_SAMPLES = 4;

  // Debounce в ISR: фронт починає нову деталь, якщо перед ним лінія мовчала 30 мс
  // і стояла у стані «відпущено».
  static constexpr TickType_t PART_QUIET_MS = 30;
  static constexpr uint32_t   IRQ_PRIO      = 6;  // не вище за configLIBRARY_MAX_SYSCALL (5)

  static constexpr uint32_t REPORT_PERIOD_MS = 1000;
  static constexpr uint32_t READY_TIMEOUT_MS = 1500;  // adc: звіт Task 1 не прийшов
  static constexpr uint32_t MUTEX_TIMEOUT_MS = 100;
  static constexpr uint32_t RATE_WINDOW_S    = 10;    // швидкість за останні 10 с
  static constexpr size_t   REPORT_SIZE      = 64;

  static constexpr UBaseType_t PRIO_REPORT = 2;
  static constexpr UBaseType_t PRIO_ADC    = 2;
  static constexpr UBaseType_t PRIO_ESTOP  = 4;
  static constexpr configSTACK_DEPTH_TYPE STACK_WORDS = 384;  // у словах: 1,5 КБ

  static constexpr uint32_t LOG_BAUD = 115200;
};

// GPIOA, GPIOB і GPIOC - адреси через приведення типу, constexpr їх не приймає.
static GPIO_TypeDef *const PART_PORT = GPIOB;
static GPIO_TypeDef *const STOP_PORT = GPIOA;
static GPIO_TypeDef *const LED_PORT  = GPIOC;
static GPIO_TypeDef *const POT_PORT  = GPIOA;
static GPIO_TypeDef *const LOG_PORT  = GPIOA;

static_assert(Config::READY_TIMEOUT_MS > Config::REPORT_PERIOD_MS,
              "adc must wait longer than one report period");
static_assert(Config::MUTEX_TIMEOUT_MS < Config::REPORT_PERIOD_MS,
              "a mutex timeout must fit into one report period");
static_assert(Config::RATE_WINDOW_S > 0, "rate window must not be empty");
static_assert(Config::PRIO_ESTOP < configMAX_PRIORITIES, "priority out of range");
static_assert(Config::PRIO_ESTOP > Config::PRIO_REPORT && Config::PRIO_ESTOP > Config::PRIO_ADC,
              "estop must outrank the conveyor tasks");
static_assert(Config::IRQ_PRIO >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY,
              "ISR that calls FromISR API must not outrank configMAX_SYSCALL_INTERRUPT_PRIORITY");

// ---------------------------------------------------------------------------
// Спільні дані.
// ---------------------------------------------------------------------------
static volatile uint32_t parts;  // лише в критичній секції

static SemaphoreHandle_t report_mutex;
static char              report_text[Config::REPORT_SIZE];  // лише під report_mutex
// Пише тільки estop, один раз і до спроби взяти mutex: зупинка не чекає на mutex.
// report і adc читають прапорець під mutex, разом із записом буфера.
static volatile bool     halted;

static SemaphoreHandle_t report_ready;  // report -> adc: «рядок записано, дописуйте»
static SemaphoreHandle_t stop_signal;   // stop_isr -> estop

// Такт DWT першого фронту KEY. Пише ISR, читає estop після семафора.
static volatile uint32_t stop_cycles;
static volatile bool     stop_seen;

static UART_HandleTypeDef huart1;
static SemaphoreHandle_t  uart_mutex;
static ADC_HandleTypeDef  hadc1;  // тільки задача adc

static void Error_Handler(void);

// ---------------------------------------------------------------------------
// Лог у форматі ESP-IDF: рівень, час у мс, тег, текст.
// Тік береться до mutex, тому в лозі момент події, а не момент, коли звільнився UART.
// ---------------------------------------------------------------------------
static void log_line(char level, const char *tag, const char *fmt, ...)
{
  char line[96];
  int  n = snprintf(line, sizeof line, "%c (%" PRIu32 ") %s: ", level,
                    (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS), tag);

  va_list args;
  va_start(args, fmt);
  n += vsnprintf(line + n, sizeof line - n, fmt, args);
  va_end(args);

  if (n > (int)sizeof line - 3) {
    n = sizeof line - 3;
  }
  line[n++] = '\r';
  line[n++] = '\n';

  if (xSemaphoreTake(uart_mutex, pdMS_TO_TICKS(Config::MUTEX_TIMEOUT_MS)) == pdTRUE) {
    HAL_UART_Transmit(&huart1, (uint8_t *)line, n, HAL_MAX_DELAY);
    xSemaphoreGive(uart_mutex);
  }
}

static uint32_t cycles_to_us(uint32_t cycles)
{
  return cycles / (SystemCoreClock / 1000000);
}

// ---------------------------------------------------------------------------
// ISR. Лише час, рівень піна, лічильник і семафор. Лог і обчислення - у задачах.
// ---------------------------------------------------------------------------
static void part_isr(void)
{
  // Ці дві змінні бачить тільки ISR, тому захист їм не потрібен.
  static TickType_t    last_edge   = 0;
  static GPIO_PinState quiet_level = GPIO_PIN_SET;  // рівень після останнього фронту

  TickType_t    now   = xTaskGetTickCountFromISR();
  GPIO_PinState level = HAL_GPIO_ReadPin(PART_PORT, Config::PART_PIN);

  // Нова деталь: лінія мовчала 30 мс і стояла «відпущено». Брязкіт при натисканні і при
  // відпусканні дає фронти частіше за 30 мс, тому другу деталь він не додає.
  if (now - last_edge >= pdMS_TO_TICKS(Config::PART_QUIET_MS) && quiet_level == GPIO_PIN_SET) {
    UBaseType_t saved = taskENTER_CRITICAL_FROM_ISR();
    parts = parts + 1;  // Read-Modify-Write, тому в критичній секції
    taskEXIT_CRITICAL_FROM_ISR(saved);
  }
  // Після останнього фронту серії рівень більше не змінюється. Тому це значення є
  // рівнем лінії під час наступної паузи.
  last_edge   = now;
  quiet_level = level;
}

static void stop_isr(void)
{
  if (!stop_seen) {
    stop_cycles = DWT->CYCCNT;  // брязкіт не переписує такт першого фронту
    stop_seen   = true;
  }
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(stop_signal, &woken);
  portYIELD_FROM_ISR(woken);  // estop отримає процесор одразу після виходу з ISR
}

extern "C" void EXTI15_10_IRQHandler(void)
{
  // Скинути до читання піна: новий фронт викличе ISR знову.
  __HAL_GPIO_EXTI_CLEAR_IT(Config::PART_PIN);
  part_isr();
}

extern "C" void EXTI0_IRQHandler(void)
{
  __HAL_GPIO_EXTI_CLEAR_IT(Config::STOP_PIN);
  stop_isr();
}

static uint32_t read_parts(void)
{
  taskENTER_CRITICAL();
  uint32_t n = parts;
  taskEXIT_CRITICAL();
  return n;
}

// ---------------------------------------------------------------------------
// Task 1: лічильник -> швидкість -> початок рядка звіту.
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
      log_line('W', "report", "mutex timeout, report skipped");
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
      log_line('W', "report", "conveyor halted, task stopped");
      vTaskDelete(nullptr);
    }
    xSemaphoreGive(report_ready);
  }
}

// ---------------------------------------------------------------------------
// Task 2: АЦП -> кінець рядка звіту -> вивід.
// ---------------------------------------------------------------------------
static bool adc_average(uint32_t channel, int samples, uint32_t *out)
{
  ADC_ChannelConfTypeDef ch = {};
  ch.Channel      = channel;
  ch.Rank         = 1;
  ch.SamplingTime = ADC_SAMPLETIME_480CYCLES;  // VREFINT потребує не менше 10 мкс
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

// Напруга на повзунку в мВ. VDDA береться з VREFINT і калібрування VREFINT_CAL:
// живлення від USB рідко дає рівно 3,3 В. Помилка АЦП: -1.
static int32_t read_pot_mv(void)
{
  uint32_t vref = 0;
  uint32_t raw  = 0;
  if (!adc_average(ADC_CHANNEL_VREFINT, Config::REF_SAMPLES, &vref) ||
      !adc_average(Config::POT_CHANNEL, Config::POT_SAMPLES, &raw) || vref == 0) {
    return -1;
  }
  uint32_t vdda_mv = VREFINT_CAL_VREF * *VREFINT_CAL_ADDR / vref;
  return (int32_t)(raw * vdda_mv / 4095);
}

static void adc_task(void *)
{
  for (;;) {
    bool fresh = xSemaphoreTake(report_ready, pdMS_TO_TICKS(Config::READY_TIMEOUT_MS)) == pdTRUE;

    // АЦП і форматування - до mutex. 20 вимірів довші за копіювання рядка.
    char tail[24];
    snprintf(tail, sizeof(tail), "; pot %" PRId32 " mV", read_pot_mv());

    if (xSemaphoreTake(report_mutex, pdMS_TO_TICKS(Config::MUTEX_TIMEOUT_MS)) != pdTRUE) {
      log_line('W', "adc", "mutex timeout, ADC value skipped");
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

    // Лог після Give, з копії буфера: log_line бере uart_mutex, а два mutex разом
    // задача не тримає.
    if (stop) {
      log_line('W', "adc", "conveyor halted, task stopped");
      vTaskDelete(nullptr);
    }
    if (!fresh) {
      log_line('W', "adc", "no report from Task 1 for %" PRIu32 " ms", Config::READY_TIMEOUT_MS);
    } else if (!appended) {
      log_line('W', "adc", "report buffer full, ADC value skipped");
    } else {
      log_line('I', "report", "%s", copy);
    }
  }
}

// ---------------------------------------------------------------------------
// Task 3 (найвищий пріоритет): спить на семафорі і не займає процесор.
// ---------------------------------------------------------------------------
static void led_set(bool on)
{
  // Світлодіод плати підключений до 3V3 через резистор: горить, коли PC13 = 0.
  HAL_GPIO_WritePin(LED_PORT, Config::LED_PIN, on ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

static void estop_task(void *)
{
  // portMAX_DELAY тут правильний: без події у задачі немає роботи.
  xSemaphoreTake(stop_signal, portMAX_DELAY);
  uint32_t woke = DWT->CYCCNT;

  // Зупинка не залежить від mutex: світлодіод, переривання і прапорець - першими.
  led_set(true);
  HAL_NVIC_DisableIRQ(EXTI15_10_IRQn);  // датчик більше не лічить деталі
  HAL_NVIC_DisableIRQ(EXTI0_IRQn);
  halted = true;  // report і adc зупиняться на своїй наступній критичній секції

  // Якщо mutex тримає report або adc, FreeRTOS піднімає її пріоритет до 4, поки
  // вона не віддасть mutex (успадкування пріоритету).
  char copy[Config::REPORT_SIZE];
  bool written = xSemaphoreTake(report_mutex, pdMS_TO_TICKS(Config::MUTEX_TIMEOUT_MS)) == pdTRUE;
  if (written) {
    snprintf(report_text, sizeof(report_text), "SYSTEM HALTED!");
    memcpy(copy, report_text, sizeof(copy));
    xSemaphoreGive(report_mutex);
  }
  uint32_t done = DWT->CYCCNT;

  if (written) {
    log_line('E', "report", "%s", copy);
  } else {
    log_line('E', "estop", "mutex timeout, SYSTEM HALTED! is not in the report buffer");
  }
  log_line('W', "estop", "parts %" PRIu32 ", ISR -> estop %" PRIu32 " us, mutex %" PRIu32 " us",
           read_parts(), cycles_to_us(woke - stop_cycles), cycles_to_us(done - woke));
  vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
// Старт: тактування -> GPIO, UART, АЦП -> mutex і семафори -> задачі -> переривання.
// ---------------------------------------------------------------------------
static void start(TaskFunction_t fn, const char *name, UBaseType_t prio)
{
  if (xTaskCreate(fn, name, Config::STACK_WORDS, nullptr, prio, nullptr) != pdPASS) {
    Error_Handler();
  }
}

static void SystemClock_Config(void);
static void gpio_init(void);
static void uart_init(void);
static void adc_init(void);
static void irq_init(void);

int main(void)
{
  HAL_Init();
  SystemClock_Config();
  gpio_init();
  uart_init();
  adc_init();

  // Лічильник тактів DWT: вимірює затримку ISR -> estop з точністю до такту (62,5 нс).
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

  uart_mutex   = xSemaphoreCreateMutex();
  report_mutex = xSemaphoreCreateMutex();
  report_ready = xSemaphoreCreateBinary();
  stop_signal  = xSemaphoreCreateBinary();
  if (uart_mutex == nullptr || report_mutex == nullptr || report_ready == nullptr ||
      stop_signal == nullptr) {
    Error_Handler();
  }

  // Планувальник ще не працює, тому рядок іде в UART напряму, без mutex.
  static const char banner[] =
      "\r\n=== dz6.5 conveyor_stm32: part sensor PB12, emergency stop KEY, alarm LED PC13 ===\r\n";
  HAL_UART_Transmit(&huart1, (uint8_t *)banner, sizeof banner - 1, HAL_MAX_DELAY);

  start(estop_task, "estop", Config::PRIO_ESTOP);
  start(adc_task, "adc", Config::PRIO_ADC);
  start(report_task, "report", Config::PRIO_REPORT);

  // xTaskCreate маскує переривання до старту планувальника, тому EXTI не спрацює раніше.
  irq_init();

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
  // HSI 16 МГц без PLL, як у ДЗ 2.8, 2.9 і 6.1-6.4.
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

static void gpio_init(void)
{
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();

  GPIO_InitTypeDef led = {};
  led.Pin   = Config::LED_PIN;
  led.Mode  = GPIO_MODE_OUTPUT_PP;
  led.Pull  = GPIO_NOPULL;
  led.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_WritePin(LED_PORT, Config::LED_PIN, GPIO_PIN_SET);  // вимкнений
  HAL_GPIO_Init(LED_PORT, &led);

  // EXTI налаштовується тут, але NVIC вмикає irq_init() після створення семафорів.
  GPIO_InitTypeDef part = {};
  part.Pin  = Config::PART_PIN;
  part.Mode = GPIO_MODE_IT_RISING_FALLING;  // debounce в ISR дивиться на обидва фронти
  part.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(PART_PORT, &part);

  GPIO_InitTypeDef stop = {};
  stop.Pin  = Config::STOP_PIN;
  stop.Mode = GPIO_MODE_IT_FALLING;  // натискання KEY = перехід 1 -> 0
  stop.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(STOP_PORT, &stop);

  GPIO_InitTypeDef pot = {};
  pot.Pin  = Config::POT_PIN;
  pot.Mode = GPIO_MODE_ANALOG;
  pot.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(POT_PORT, &pot);
}

static void irq_init(void)
{
  // Фронти, які прийшли до цього моменту, не рахуються.
  __HAL_GPIO_EXTI_CLEAR_IT(Config::PART_PIN | Config::STOP_PIN);
  HAL_NVIC_SetPriority(EXTI15_10_IRQn, Config::IRQ_PRIO, 0);
  HAL_NVIC_SetPriority(EXTI0_IRQn, Config::IRQ_PRIO, 0);
  HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);
  HAL_NVIC_EnableIRQ(EXTI0_IRQn);
}

static void uart_init(void)
{
  __HAL_RCC_USART1_CLK_ENABLE();

  GPIO_InitTypeDef tx = {};
  tx.Pin       = Config::LOG_TX_PIN;
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
  __HAL_RCC_ADC1_CLK_ENABLE();

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
  LED_PORT->BSRR = (uint32_t)Config::LED_PIN << 16;  // скинути PC13 = увімкнути
  while (1) {
  }
}

extern "C" void rtos_assert_failed(void) { Error_Handler(); }
extern "C" void vApplicationMallocFailedHook(void) { Error_Handler(); }
extern "C" void vApplicationStackOverflowHook(TaskHandle_t, char *) { Error_Handler(); }
