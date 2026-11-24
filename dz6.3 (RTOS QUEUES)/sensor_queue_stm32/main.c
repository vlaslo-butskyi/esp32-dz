// ДЗ 6.3 на STM32F411 (Black Pill): кілька датчиків -> одна черга -> один consumer.
//
//   temp   (задача) - вбудований датчик температури (ADC1, канал 18), кожні 1000 мс;
//   pot    (задача) - потенціометр на PA1 (ADC1, канал 1), середнє з 16 вимірювань, кожні 250 мс;
//   button (ISR)    - кнопка KEY (PA0) через EXTI0, подія на кожне натискання;
//   consumer        - єдиний читач черги і єдиний власник статистики.
//
// Дані між задачами йдуть тільки через чергу. Два м'ютекси захищають периферію, а не дані:
// ADC1 спільний для temp і pot, USART1 - для всіх, хто пише в лог.
// Облік повідомлень, фільтр натискань і рядки логу - у ../common, спільні з ESP32-версією.

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>

#include "stm32f4xx_hal.h"
#include "stm32f4xx_ll_adc.h"  // адреси калібрувань VREFINT_CAL і TS_CAL у системній пам'яті

#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"

#include "consumer.h"
#include "press_filter.h"
#include "sensor_msg.h"

enum {
  POT_SAMPLES     = 16,
  REF_SAMPLES     = 4,
  BUTTON_IRQ_PRIO = 6,  // не вище за configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY (5)
  // Експеримент «повільний consumer»: 0 - звичайна робота, 500 - черга переповниться.
  CONSUMER_DELAY_MS = 0,
  PRIO_PRODUCER = 2,
  PRIO_CONSUMER = 3,
  STACK_WORDS   = 256,  // 1 КБ
  LOG_BAUD      = 115200,
};

_Static_assert(PRIO_PRODUCER < PRIO_CONSUMER, "consumer must outrank producers");
_Static_assert(BUTTON_IRQ_PRIO >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY,
               "ISR that calls FromISR API must not outrank configMAX_SYSCALL_INTERRUPT_PRIORITY");

static const uint32_t      POT_CHANNEL = ADC_CHANNEL_1;
static GPIO_TypeDef *const LOG_PORT    = GPIOA;
static const uint16_t      LOG_TX_PIN  = GPIO_PIN_9;   // USART1_TX, AF7
static GPIO_TypeDef *const POT_PORT    = GPIOA;
static const uint16_t      POT_PIN     = GPIO_PIN_1;
static GPIO_TypeDef *const BUTTON_PORT = GPIOA;
static const uint16_t      BUTTON_PIN  = GPIO_PIN_0;   // KEY на платі, натиснута = 0
static GPIO_TypeDef *const LED_PORT    = GPIOC;
static const uint16_t      LED_PIN     = GPIO_PIN_13;  // світлодіод плати, горить від 0

static UART_HandleTypeDef huart1;
static ADC_HandleTypeDef  hadc1;
static SemaphoreHandle_t  uart_mutex;
static SemaphoreHandle_t  adc_mutex;
static QueueHandle_t      queue;  // створюється першим, до датчиків, задач і переривання

static void Error_Handler(void);

static uint32_t now_ms(void)
{
  return xTaskGetTickCount() * portTICK_PERIOD_MS;
}

// ---------------------------------------------------------------------------
// Лог: тік береться до м'ютекса, тому в лозі момент події, а не момент, коли звільнився UART.
// ---------------------------------------------------------------------------
static void log_line(const char *fmt, ...)
{
  char line[112];
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
// Датчики. Обидва читають ADC1, тому кожне читання - під adc_mutex.
// ---------------------------------------------------------------------------
static bool adc_average(uint32_t channel, int samples, uint32_t *out)
{
  ADC_ChannelConfTypeDef ch = {
    .Channel      = channel,
    .Rank         = 1,
    .SamplingTime = ADC_SAMPLETIME_480CYCLES,  // датчику температури треба не менше 10 мкс
  };
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
  bool ok = adc_average(ADC_CHANNEL_VREFINT, REF_SAMPLES, &vref) &&
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
  if (!adc_read(POT_CHANNEL, POT_SAMPLES, &raw, &vdda)) {
    return false;
  }
  *out = (int32_t)(raw * vdda / 4095);
  return true;
}

// ---------------------------------------------------------------------------
// Продюсери: одна функція на всі датчики, параметр - рядок таблиці.
// ---------------------------------------------------------------------------
typedef struct {
  source_t source;
  bool (*read)(int32_t *out);
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
    if (!p->read(&value)) {
      log_line("%-6s read error", info->name);
      continue;
    }

    sensor_msg_t msg = {
      .source  = p->source,
      .core    = 0,      // ядро одне
      .seq     = seq++,  // росте і при невдалій відправці: consumer побачить розрив
      .time_ms = last_wake * portTICK_PERIOD_MS,
      .value   = value,
    };

    // Обмежений таймаут: якщо consumer завис, датчик не зупиняється, а втрата видна в лозі.
    if (xQueueSend(queue, &msg, pdMS_TO_TICKS(SEND_TIMEOUT_MS)) != pdPASS) {
      log_line("%-6s queue full, #%" PRIu32 " dropped", info->name, msg.seq);
    }
  }
}

// ---------------------------------------------------------------------------
// ISR кнопки. Тільки ...FromISR API, без логу.
// ---------------------------------------------------------------------------

// Єдина змінна поза чергою: ISR не може надрукувати «queue full», тому рахує втрати тут.
// Пише тільки ISR, читає тільки consumer. Запис і читання 32 біт на Cortex-M4 атомарні.
static volatile uint32_t button_dropped;

static press_filter_t button_filter;  // чіпає тільки ISR після button_init
static uint32_t       button_seq;

static void button_isr(void)
{
  uint32_t now = xTaskGetTickCountFromISR() * portTICK_PERIOD_MS;
  bool released = HAL_GPIO_ReadPin(BUTTON_PORT, BUTTON_PIN) == GPIO_PIN_SET;
  if (!press_filter_edge(&button_filter, released, now)) {
    return;
  }

  sensor_msg_t msg = {
    .source  = SOURCE_BUTTON,
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

void EXTI0_IRQHandler(void)
{
  __HAL_GPIO_EXTI_CLEAR_IT(BUTTON_PIN);  // скинути до читання піна: новий фронт викличе ISR знову
  button_isr();
}

static void button_init(void)
{
  GPIO_InitTypeDef gpio = {
    .Pin  = BUTTON_PIN,
    .Mode = GPIO_MODE_IT_RISING_FALLING,
    .Pull = GPIO_PULLUP,
  };
  HAL_GPIO_Init(BUTTON_PORT, &gpio);
  press_filter_init(&button_filter, HAL_GPIO_ReadPin(BUTTON_PORT, BUTTON_PIN) == GPIO_PIN_SET);

  HAL_NVIC_SetPriority(EXTI0_IRQn, BUTTON_IRQ_PRIO, 0);
  HAL_NVIC_EnableIRQ(EXTI0_IRQn);
}

// ---------------------------------------------------------------------------
// Consumer
// ---------------------------------------------------------------------------
static void log_print(const char *line, void *ctx)
{
  (void)ctx;
  log_line("%s", line);
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
// Старт: черга -> периферія -> consumer -> продюсери -> переривання -> планувальник.
// ---------------------------------------------------------------------------
static void start(TaskFunction_t fn, const char *name, void *arg, UBaseType_t prio)
{
  if (xTaskCreate(fn, name, STACK_WORDS, arg, prio, NULL) != pdPASS) {
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

  queue      = xQueueCreate(QUEUE_LENGTH, sizeof(sensor_msg_t));
  uart_mutex = xSemaphoreCreateMutex();
  adc_mutex  = xSemaphoreCreateMutex();
  if (queue == NULL || uart_mutex == NULL || adc_mutex == NULL) {
    Error_Handler();
  }

  adc_init();

  // Планувальник ще не працює, тому рядок іде в UART напряму, без м'ютекса.
  static const char banner[] =
      "\r\n=== dz6.3 sensor_queue_stm32: temp, pot, KEY -> queue of 8 -> consumer ===\r\n";
  HAL_UART_Transmit(&huart1, (uint8_t *)banner, sizeof banner - 1, HAL_MAX_DELAY);

  start(consumer_task, "consumer", NULL, PRIO_CONSUMER);
  for (size_t i = 0; i < sizeof PRODUCERS / sizeof PRODUCERS[0]; ++i) {
    start(producer_task, SOURCES[PRODUCERS[i].source].name, (void *)&PRODUCERS[i], PRIO_PRODUCER);
  }

  // xTaskCreate маскує переривання до старту планувальника, тому EXTI0 не спрацює раніше.
  button_init();

  vTaskStartScheduler();

  Error_Handler();  // сюди потрапляємо, лише якщо не вистачило купи на idle-задачу
}

// SysTick потрібен обом: HAL рахує в ньому HAL_GetTick() (на ньому тримаються
// тайм-аути HAL_RCC, HAL_UART і HAL_ADC), а FreeRTOS - свій тік.
void xPortSysTickHandler(void);

void SysTick_Handler(void)
{
  HAL_IncTick();
  if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
    xPortSysTickHandler();
  }
}

static void SystemClock_Config(void)
{
  // HSI 16 МГц без PLL, як у ДЗ 2.8, 2.9, 6.1 і 6.2.
  RCC_OscInitTypeDef osc = {
    .OscillatorType      = RCC_OSCILLATORTYPE_HSI,
    .HSIState            = RCC_HSI_ON,
    .HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT,
    .PLL.PLLState        = RCC_PLL_NONE,
  };
  if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
    Error_Handler();
  }

  RCC_ClkInitTypeDef clk = {
    .ClockType      = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_PCLK1 |
                      RCC_CLOCKTYPE_PCLK2,
    .SYSCLKSource   = RCC_SYSCLKSOURCE_HSI,
    .AHBCLKDivider  = RCC_SYSCLK_DIV1,
    .APB1CLKDivider = RCC_HCLK_DIV1,
    .APB2CLKDivider = RCC_HCLK_DIV1,
  };
  if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_0) != HAL_OK) {
    Error_Handler();
  }
}

static void led_init(void)
{
  __HAL_RCC_GPIOC_CLK_ENABLE();

  GPIO_InitTypeDef gpio = {
    .Pin   = LED_PIN,
    .Mode  = GPIO_MODE_OUTPUT_PP,
    .Pull  = GPIO_NOPULL,
    .Speed = GPIO_SPEED_FREQ_LOW,
  };
  HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_SET);  // вимкнений
  HAL_GPIO_Init(LED_PORT, &gpio);
}

static void uart_init(void)
{
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_USART1_CLK_ENABLE();

  GPIO_InitTypeDef tx = {
    .Pin       = LOG_TX_PIN,
    .Mode      = GPIO_MODE_AF_PP,
    .Pull      = GPIO_NOPULL,
    .Speed     = GPIO_SPEED_FREQ_HIGH,
    .Alternate = GPIO_AF7_USART1,
  };
  HAL_GPIO_Init(LOG_PORT, &tx);

  huart1.Instance          = USART1;
  huart1.Init.BaudRate     = LOG_BAUD;
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

  GPIO_InitTypeDef pot = {
    .Pin  = POT_PIN,
    .Mode = GPIO_MODE_ANALOG,
    .Pull = GPIO_NOPULL,
  };
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

void rtos_assert_failed(void) { Error_Handler(); }
void vApplicationMallocFailedHook(void) { Error_Handler(); }
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
  (void)task;
  (void)name;
  Error_Handler();
}
