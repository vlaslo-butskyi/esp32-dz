#include <stdarg.h>
#include <stdio.h>

#include "stm32f4xx_hal.h"

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

struct Config {
  static constexpr UBaseType_t PRIO_LOW      = 1;
  static constexpr UBaseType_t PRIO_NORMAL   = 2;
  static constexpr UBaseType_t PRIO_HIGH     = 3;
  static constexpr UBaseType_t PRIO_REALTIME = 4;

  static constexpr uint32_t SENSOR_PERIOD_MS      = 500;
  static constexpr uint32_t EVENT_PERIOD_MS       = 3000;
  static constexpr uint32_t MAINTENANCE_PERIOD_MS = 10000;
  static constexpr uint32_t MAINTENANCE_PAUSE_MS  = 2000;

  static constexpr configSTACK_DEPTH_TYPE STACK_WORDS = 256;  // у словах: 1 КБ

  static constexpr uint32_t LOG_BAUD = 115200;
};

#define LOG_PORT   GPIOA
#define LOG_TX_PIN GPIO_PIN_9   // USART1_TX, AF7
#define LED_PORT   GPIOC
#define LED_PIN    GPIO_PIN_13  // світлодіод плати, горить від низького рівня

static UART_HandleTypeDef huart1;
static SemaphoreHandle_t  uart_mutex;

static void Error_Handler(void);

// Тік береться до м'ютекса: у лозі момент події, а не момент, коли звільнився UART.
// HAL_UART_Transmit чекає, поки вийде весь рядок, тому без м'ютекса рядки
// задач перемішались би.
static void log_line(const char *fmt, ...)
{
  char line[80];
  int n = snprintf(line, sizeof line, "%7lu  ",
                   (unsigned long)(xTaskGetTickCount() * portTICK_PERIOD_MS));

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

// Task 1, Normal: опитування датчика строго кожні 500 мс.
static void sensor_task(void *)
{
  const TickType_t period = pdMS_TO_TICKS(Config::SENSOR_PERIOD_MS);
  TickType_t last_wake = xTaskGetTickCount();

  for (;;) {
    vTaskDelayUntil(&last_wake, period);

    // Після паузи пропускаємо моменти, які минули, інакше вони вийдуть усі разом.
    TickType_t late = xTaskGetTickCount() - last_wake;
    last_wake += late - late % period;

    log_line("[Sensor] Data read...");
  }
}

// Task 3, High: живе рівно одну подію.
static void worker_task(void *)
{
  log_line("[Worker] Processing event...");
  vTaskDelete(nullptr);
}

// Task 2, Low: раз на 3 с створює Worker Task.
static void event_task(void *)
{
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(Config::EVENT_PERIOD_MS));

    if (xTaskCreate(worker_task, "worker", Config::STACK_WORDS, nullptr,
                    Config::PRIO_HIGH, nullptr) != pdPASS) {
      log_line("[Event] no memory for Worker Task");
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
    log_line("[Maintenance] System paused for %lu seconds.",
             (unsigned long)(Config::MAINTENANCE_PAUSE_MS / 1000));
    vTaskDelay(pdMS_TO_TICKS(Config::MAINTENANCE_PAUSE_MS));
    vTaskResume(sensor);
  }
}

static void start(TaskFunction_t fn, const char *name, UBaseType_t prio, void *arg,
                  TaskHandle_t *handle)
{
  if (xTaskCreate(fn, name, Config::STACK_WORDS, arg, prio, handle) != pdPASS) {
    Error_Handler();
  }
}

static void SystemClock_Config(void);
static void led_init(void);
static void uart_init(void);

int main(void)
{
  HAL_Init();
  SystemClock_Config();
  led_init();
  uart_init();

  uart_mutex = xSemaphoreCreateMutex();
  if (uart_mutex == nullptr) {
    Error_Handler();
  }

  // Планувальник ще не працює, тому рядок іде в UART напряму, без м'ютекса.
  static const char banner[] = "\r\n=== dz6.2 four_tasks_stm32 ===\r\n";
  HAL_UART_Transmit(&huart1, (uint8_t *)banner, sizeof banner - 1, HAL_MAX_DELAY);

  TaskHandle_t sensor = nullptr;
  start(sensor_task,      "sensor",      Config::PRIO_NORMAL,   nullptr, &sensor);
  start(event_task,       "event",       Config::PRIO_LOW,      nullptr, nullptr);
  start(maintenance_task, "maintenance", Config::PRIO_REALTIME, sensor,  nullptr);

  vTaskStartScheduler();

  Error_Handler();  // сюди потрапляємо, лише якщо не вистачило купи на idle-задачу
}

// SysTick потрібен обом: HAL рахує в ньому HAL_GetTick() (на ньому тримаються
// тайм-аути HAL_RCC і HAL_UART), а FreeRTOS - свій тік.
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
  // HSI 16 МГц без PLL, як у ДЗ 2.8, 2.9 і 6.1.
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
