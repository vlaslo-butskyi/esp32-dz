// ДЗ 6.4 на STM32F411 (Black Pill): електронний сейф на кінцевому автоматі.
// Пароль: Коротке - Коротке - Довге.
//
//   button (задача, Producer) - опитує KEY (PA0) кожні 10 мс і передає рівень автомату кнопки;
//   safe   (задача, Consumer) - читає чергу, передає подію автомату сейфа, керує світлодіодом
//                               плати (PC13), пише лог у USART1 (PA9).
//
// Обидва автомати лежать у ../common і не знають ні про FreeRTOS, ні про HAL.
// Тут тільки периферія, черга, задачі й лог.

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>

#include "stm32f4xx_hal.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"

#include "button_fsm.h"
#include "safe_fsm.h"

enum {
  QUEUE_LENGTH = 8,
  PRIO_BUTTON  = 3,    // опитування не чекає на лог у UART
  PRIO_SAFE    = 2,
  STACK_WORDS  = 256,  // 1 КБ
  LOG_BAUD     = 115200,
};

_Static_assert(PRIO_BUTTON > PRIO_SAFE, "button polling must not wait for the safe");
_Static_assert(PRIO_BUTTON < configMAX_PRIORITIES, "priority out of range");

static GPIO_TypeDef *const BUTTON_PORT = GPIOA;
static const uint16_t      BUTTON_PIN  = GPIO_PIN_0;   // KEY на платі, натиснута = 0
static GPIO_TypeDef *const LED_PORT    = GPIOC;
static const uint16_t      LED_PIN     = GPIO_PIN_13;  // світлодіод плати, горить від 0
static GPIO_TypeDef *const LOG_PORT    = GPIOA;
static const uint16_t      LOG_TX_PIN  = GPIO_PIN_9;   // USART1_TX, AF7

static UART_HandleTypeDef huart1;
static SemaphoreHandle_t  uart_mutex;
static QueueHandle_t      queue;

static void Error_Handler(void);

static uint32_t now_ms(void)
{
  return xTaskGetTickCount() * portTICK_PERIOD_MS;
}

// ---------------------------------------------------------------------------
// Лог у форматі ESP-IDF: рівень (I або W), час у мс, текст.
// Тік береться до м'ютекса, тому в лозі момент події, а не момент, коли звільнився UART.
// ---------------------------------------------------------------------------
static void log_line(char level, const char *fmt, ...)
{
  char line[112];
  int n = snprintf(line, sizeof line, "%c (%" PRIu32 ") safe: ", level, now_ms());

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
// Producer
// ---------------------------------------------------------------------------
static bool queue_send(const event_msg_t *msg, void *ctx)
{
  (void)ctx;
  // Не чекати: якщо сейф не встигає, кнопка не повинна пропустити фронт.
  // Невдачу button_poll перетворить на EVENT_OVERFLOW.
  bool ok = xQueueSend(queue, msg, 0) == pdPASS;
  if (!ok) {
    log_line('W', "queue full, %s dropped", event_name(msg->event));
  }
  return ok;
}

static void button_task(void *arg)
{
  (void)arg;
  button_fsm_t button;
  TickType_t   last_wake = xTaskGetTickCount();
  button_init(&button, now_ms());

  for (;;) {
    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(BUTTON_POLL_MS));
    bool pressed = HAL_GPIO_ReadPin(BUTTON_PORT, BUTTON_PIN) == GPIO_PIN_RESET;
    button_poll(&button, pressed, now_ms(), queue_send, NULL);
  }
}

// ---------------------------------------------------------------------------
// Consumer
// ---------------------------------------------------------------------------
static void led_set(bool on)
{
  // Світлодіод плати підключений до 3V3 через резистор: горить, коли PC13 = 0.
  HAL_GPIO_WritePin(LED_PORT, LED_PIN, on ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

static void safe_task(void *arg)
{
  (void)arg;
  safe_t safe;
  safe_init(&safe, now_ms());

  for (;;) {
    uint32_t   wait  = safe_wait_ms(&safe, now_ms());
    TickType_t ticks = wait == SAFE_WAIT_FOREVER ? portMAX_DELAY : pdMS_TO_TICKS(wait);

    event_msg_t msg;
    if (xQueueReceive(queue, &msg, ticks) != pdPASS) {
      msg = (event_msg_t){EVENT_TIMEOUT, 0};
    }

    safe_state_t      from = safe.state;
    safe_transition_t t    = safe_handle(&safe, &msg, now_ms());
    // Світлодіод - до логу: HAL_UART_Transmit займає кілька мс.
    if (t.next != from) {
      led_set(t.next == SAFE_OPEN);
    }

    char line[96];
    char level = safe_describe(line, sizeof line, &msg, from, t);
    log_line(level, "%s", line);
  }
}

// ---------------------------------------------------------------------------
// Старт: тактування -> GPIO -> UART -> черга -> задачі -> планувальник.
// ---------------------------------------------------------------------------
static void start(TaskFunction_t fn, const char *name, UBaseType_t prio)
{
  if (xTaskCreate(fn, name, STACK_WORDS, NULL, prio, NULL) != pdPASS) {
    Error_Handler();
  }
}

static void SystemClock_Config(void);
static void gpio_init(void);
static void uart_init(void);

int main(void)
{
  HAL_Init();
  SystemClock_Config();
  gpio_init();
  uart_init();

  queue      = xQueueCreate(QUEUE_LENGTH, sizeof(event_msg_t));
  uart_mutex = xSemaphoreCreateMutex();
  if (queue == NULL || uart_mutex == NULL) {
    Error_Handler();
  }

  // Планувальник ще не працює, тому рядок іде в UART напряму, без м'ютекса.
  static const char banner[] =
      "\r\n=== dz6.4 safe_fsm_stm32: password SHORT SHORT LONG (>= 1000 ms), "
      "timeout 3000 ms ===\r\n";
  HAL_UART_Transmit(&huart1, (uint8_t *)banner, sizeof banner - 1, HAL_MAX_DELAY);

  start(safe_task, "safe", PRIO_SAFE);
  start(button_task, "button", PRIO_BUTTON);

  vTaskStartScheduler();

  Error_Handler();  // сюди потрапляємо, лише якщо не вистачило купи на idle-задачу
}

// SysTick потрібен обом: HAL рахує в ньому HAL_GetTick() (на ньому тримаються
// тайм-аути HAL_RCC і HAL_UART), а FreeRTOS - свій тік.
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
  // HSI 16 МГц без PLL, як у ДЗ 2.8, 2.9 і 6.1-6.3.
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

static void gpio_init(void)
{
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();

  GPIO_InitTypeDef led = {
    .Pin   = LED_PIN,
    .Mode  = GPIO_MODE_OUTPUT_PP,
    .Pull  = GPIO_NOPULL,
    .Speed = GPIO_SPEED_FREQ_LOW,
  };
  HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_SET);  // вимкнений
  HAL_GPIO_Init(LED_PORT, &led);

  GPIO_InitTypeDef button = {
    .Pin  = BUTTON_PIN,
    .Mode = GPIO_MODE_INPUT,
    .Pull = GPIO_PULLUP,
  };
  HAL_GPIO_Init(BUTTON_PORT, &button);
}

static void uart_init(void)
{
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
