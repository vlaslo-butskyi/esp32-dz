#include "stm32f4xx_hal.h"

#include "FreeRTOS.h"
#include "task.h"

struct Config {
  static constexpr uint16_t LED_RED_PIN    = GPIO_PIN_12;
  static constexpr uint16_t LED_GREEN_PIN  = GPIO_PIN_13;
  static constexpr uint16_t LED_YELLOW_PIN = GPIO_PIN_14;

  static constexpr uint32_t LED_RED_INTERVAL_MS    = 200;
  static constexpr uint32_t LED_GREEN_INTERVAL_MS  = 500;
  static constexpr uint32_t LED_YELLOW_INTERVAL_MS = 1000;

  static constexpr UBaseType_t    LED_PRIORITY    = 3;
  static constexpr configSTACK_DEPTH_TYPE LED_STACK_WORDS = 128;  // у словах: 512 байт
};

// Усі три світлодіоди на порту B. GPIOB - це приведення адреси до вказівника,
// тому в constexpr-структуру воно не входить.
#define LED_PORT GPIOB

struct Blinker {
  const char *name;
  uint16_t    pin;
  uint32_t    interval_ms;
};

static const Blinker LEDS[] = {
  { "red",    Config::LED_RED_PIN,    Config::LED_RED_INTERVAL_MS    },
  { "green",  Config::LED_GREEN_PIN,  Config::LED_GREEN_INTERVAL_MS  },
  { "yellow", Config::LED_YELLOW_PIN, Config::LED_YELLOW_INTERVAL_MS },
};

static void SystemClock_Config(void);
static void Error_Handler(void);

// Та сама функція, що в ESP32-версії. Пін задача не налаштовує: це робить
// main() до старту планувальника, бо HAL_GPIO_Init змінює спільні регістри
// порту B, і дві задачі могли б перебити одна одну посеред запису.
static void blink_task(void *arg)
{
  const Blinker &led = *static_cast<const Blinker *>(arg);
  bool level = false;

  TickType_t last_wake = xTaskGetTickCount();
  for (;;) {
    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(led.interval_ms));

    level = !level;
    // WritePin пише в BSRR - один атомарний запис, без читання регістра.
    HAL_GPIO_WritePin(LED_PORT, led.pin, level ? GPIO_PIN_SET : GPIO_PIN_RESET);
  }
}

static void leds_init(void)
{
  __HAL_RCC_GPIOB_CLK_ENABLE();

  GPIO_InitTypeDef gpio = {};
  gpio.Pin   = Config::LED_RED_PIN | Config::LED_GREEN_PIN | Config::LED_YELLOW_PIN;
  gpio.Mode  = GPIO_MODE_OUTPUT_PP;
  gpio.Pull  = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_WritePin(LED_PORT, gpio.Pin, GPIO_PIN_RESET);
  HAL_GPIO_Init(LED_PORT, &gpio);
}

int main(void)
{
  HAL_Init();
  SystemClock_Config();
  leds_init();

  // Ядро одне, тож xTaskCreatePinnedToCore тут немає - тільки xTaskCreate.
  for (const Blinker &led : LEDS) {
    if (xTaskCreate(
          blink_task,
          led.name,
          Config::LED_STACK_WORDS,
          (void *)&led,
          Config::LED_PRIORITY,
          nullptr
        ) != pdPASS) {
      Error_Handler();
    }
  }

  // На ESP32 планувальник запускає ESP-IDF ще до app_main. Тут це робить
  // main(), і з vTaskStartScheduler() керування вже не повертається.
  vTaskStartScheduler();

  Error_Handler();  // сюди потрапляємо, лише якщо не вистачило купи на idle-задачу
}

// SysTick потрібен обом: HAL рахує в ньому HAL_GetTick() (на ньому тримаються
// тайм-аути в HAL_RCC_OscConfig), а FreeRTOS - свій тік. До старту
// планувальника FreeRTOS викликати не можна, тому перевірка стану.
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
  // HSI 16 МГц без PLL, як у ДЗ 2.8 і 2.9.
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

// Будь-яка фатальна помилка: усі три світлодіоди горять постійно.
static void Error_Handler(void)
{
  __disable_irq();
  LED_PORT->BSRR = Config::LED_RED_PIN | Config::LED_GREEN_PIN | Config::LED_YELLOW_PIN;
  while (1) {
  }
}

extern "C" void dz61_assert_failed(void) { Error_Handler(); }
extern "C" void vApplicationMallocFailedHook(void) { Error_Handler(); }
extern "C" void vApplicationStackOverflowHook(TaskHandle_t, char *) { Error_Handler(); }
