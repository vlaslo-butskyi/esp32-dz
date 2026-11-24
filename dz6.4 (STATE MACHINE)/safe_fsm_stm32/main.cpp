// ДЗ 6.4 на STM32F411 (Black Pill): електронний сейф на кінцевому автоматі.
// Пароль: Коротке - Коротке - Довге.
//
//   button (задача, Producer) - автомат кнопки: опитує KEY (PA0) кожні 10 мс, придушує брязкіт
//                               і кладе в чергу події PRESS, SHORT, LONG;
//   safe   (задача, Consumer) - автомат сейфа: читає чергу, перемикає стан сейфа і світлодіод
//                               плати (PC13), пише лог у USART1 (PA9).
//
// Автомати ті самі, що в ESP32-версії safe_fsm: змінилися тільки GPIO, лог і старт.

#include <inttypes.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

#include "stm32f4xx_hal.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"

struct Config {
  static constexpr uint32_t POLL_MS     = 10;    // період опитування кнопки
  static constexpr uint32_t DEBOUNCE_MS = 30;    // стільки рівень має не змінюватися
  static constexpr uint32_t LONG_MS     = 1000;  // утримання від цього часу = LONG

  static constexpr uint32_t INPUT_TIMEOUT_MS = 3000;  // «затупив»: тиша між подіями
  static constexpr uint32_t OPEN_MS          = 5000;  // сейф сам закривається

  static constexpr UBaseType_t QUEUE_LENGTH = 8;
  static constexpr UBaseType_t PRIO_BUTTON  = 3;  // опитування не чекає на лог у UART
  static constexpr UBaseType_t PRIO_SAFE    = 2;
  static constexpr configSTACK_DEPTH_TYPE STACK_WORDS = 256;  // у словах: 1 КБ

  static constexpr uint32_t LOG_BAUD = 115200;

  static constexpr uint16_t BUTTON_PIN = GPIO_PIN_0;   // KEY на платі, натиснута = 0
  static constexpr uint16_t LED_PIN    = GPIO_PIN_13;  // світлодіод плати, горить від 0
  static constexpr uint16_t LOG_TX_PIN = GPIO_PIN_9;   // USART1_TX, AF7
};

// GPIOA і GPIOC - адреси через приведення типу, constexpr їх не приймає.
static GPIO_TypeDef *const BUTTON_PORT = GPIOA;
static GPIO_TypeDef *const LED_PORT    = GPIOC;
static GPIO_TypeDef *const LOG_PORT    = GPIOA;

static_assert(Config::DEBOUNCE_MS >= 2 * Config::POLL_MS, "debounce needs at least 2 polls");
static_assert(Config::LONG_MS > Config::DEBOUNCE_MS, "a long press must outlast debounce");
static_assert(Config::INPUT_TIMEOUT_MS > Config::LONG_MS,
              "LONG must arrive before the input timeout, counted from PRESS");
static_assert(Config::PRIO_BUTTON < configMAX_PRIORITIES, "priority out of range");

static UART_HandleTypeDef huart1;
static SemaphoreHandle_t  uart_mutex;

static void Error_Handler(void);

// ---------------------------------------------------------------------------
// Лог у форматі ESP-IDF: рівень (I або W), час у мс, текст.
// Тік береться до м'ютекса, тому в лозі момент події, а не момент, коли звільнився UART.
// ---------------------------------------------------------------------------
static void log_line(char level, const char *fmt, ...)
{
  char line[96];
  int n = snprintf(line, sizeof line, "%c (%" PRIu32 ") safe: ", level,
                   (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS));

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
// Події. PRESS, SHORT і LONG кладе в чергу автомат кнопки. TIMEOUT створює сам автомат
// сейфа, коли xQueueReceive не дочекався події.
// ---------------------------------------------------------------------------
enum class Event : uint8_t { Press, Short, Long, Timeout };

struct Message {
  Event    event;
  uint32_t hold_ms;  // SHORT і LONG: скільки кнопку тримали; PRESS: 0
};

static QueueHandle_t queue;

static uint32_t ticks_to_ms(TickType_t ticks)
{
  return ticks * portTICK_PERIOD_MS;
}

// ---------------------------------------------------------------------------
// Producer: автомат кнопки.
//
//   RELEASED --натиснута--> PRESSED      [PRESS]
//   PRESSED  --відпущена--> RELEASED     [SHORT]
//   PRESSED  --1000 мс----> HELD         [LONG]
//   HELD     --відпущена--> RELEASED
//
// «Натиснута» і «відпущена» - рівень після debounce, тому брязкіт і при натисканні,
// і при відпусканні не дає зайвих подій. Увесь час - різниця тіків і порівняння через >=.
// ---------------------------------------------------------------------------
enum class ButtonState : uint8_t { Released, Pressed, Held };

static void send(Event event, uint32_t hold_ms)
{
  Message msg = {event, hold_ms};
  // Не чекати: якщо сейф не встигає, кнопка не повинна пропустити фронт.
  if (xQueueSend(queue, &msg, 0) != pdPASS) {
    log_line('W', "queue full, event dropped");
  }
}

static void button_task(void *)
{
  ButtonState state     = ButtonState::Released;
  bool        stable    = false;  // рівень після debounce: true = натиснута
  bool        candidate = false;  // останній прочитаний рівень
  TickType_t  changed   = xTaskGetTickCount();  // коли candidate змінився востаннє
  TickType_t  pressed   = 0;                    // коли почалося натискання
  TickType_t  last_wake = changed;

  for (;;) {
    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(Config::POLL_MS));
    TickType_t now = xTaskGetTickCount();

    // Debounce: новий рівень приймається, коли він тримається DEBOUNCE_MS.
    bool raw = HAL_GPIO_ReadPin(BUTTON_PORT, Config::BUTTON_PIN) == GPIO_PIN_RESET;
    if (raw != candidate) {
      candidate = raw;
      changed   = now;
    }
    bool edge = candidate != stable && now - changed >= pdMS_TO_TICKS(Config::DEBOUNCE_MS);
    if (edge) {
      stable = candidate;
    }

    switch (state) {
      case ButtonState::Released:
        if (edge && stable) {
          pressed = changed;  // перший відлік нового рівня, а не кінець debounce
          send(Event::Press, 0);
          state = ButtonState::Pressed;
        }
        break;

      case ButtonState::Pressed:
        if (edge && !stable) {
          send(Event::Short, ticks_to_ms(changed - pressed));
          state = ButtonState::Released;
        } else if (candidate && now - pressed >= pdMS_TO_TICKS(Config::LONG_MS)) {
          // candidate == false: кнопку вже відпустили до 1000 мс, debounce ще чекає.
          send(Event::Long, ticks_to_ms(now - pressed));
          state = ButtonState::Held;
        }
        break;

      case ButtonState::Held:
        if (edge && !stable) {
          state = ButtonState::Released;
        }
        break;
    }
  }
}

// ---------------------------------------------------------------------------
// Consumer: автомат сейфа.
//
//   LOCKED  --SHORT--> SHORT_1
//   SHORT_1 --SHORT--> SHORT_2
//   SHORT_2 --LONG---> OPEN
//   OPEN    --SHORT або TIMEOUT 5 с--> LOCKED
//
//   SHORT_1, SHORT_2: неправильна подія або TIMEOUT 3 с --> LOCKED
//   LOCKED: LONG --> LOCKED (неправильна подія, лічити нічого)
//   PRESS ніде не змінює стан. Він тільки перезапускає таймаут.
//
// Функція переходу чиста: без FreeRTOS і без GPIO. Тому static_assert нижче перевіряє
// її ще під час збірки.
// ---------------------------------------------------------------------------
enum class SafeState : uint8_t { Locked, Short1, Short2, Open };

enum class Outcome : uint8_t {
  Stay,     // подія нічого не змінює
  Step,     // правильний крок пароля
  Unlock,   // пароль введено
  Wrong,    // неправильна подія, скидання
  Expired,  // таймаут, скидання
  Close,    // сейф закрито
};

struct Transition {
  SafeState next;
  Outcome   outcome;
};

static constexpr Transition safe_step(SafeState state, Event event)
{
  switch (state) {
    case SafeState::Locked:
      if (event == Event::Short) return {SafeState::Short1, Outcome::Step};
      if (event == Event::Long)  return {SafeState::Locked, Outcome::Wrong};
      return {SafeState::Locked, Outcome::Stay};

    case SafeState::Short1:
      if (event == Event::Short)   return {SafeState::Short2, Outcome::Step};
      if (event == Event::Long)    return {SafeState::Locked, Outcome::Wrong};
      if (event == Event::Timeout) return {SafeState::Locked, Outcome::Expired};
      return {SafeState::Short1, Outcome::Stay};

    case SafeState::Short2:
      if (event == Event::Long)    return {SafeState::Open,   Outcome::Unlock};
      if (event == Event::Short)   return {SafeState::Locked, Outcome::Wrong};
      if (event == Event::Timeout) return {SafeState::Locked, Outcome::Expired};
      return {SafeState::Short2, Outcome::Stay};

    case SafeState::Open:
      if (event == Event::Short || event == Event::Timeout) {
        return {SafeState::Locked, Outcome::Close};
      }
      return {SafeState::Open, Outcome::Stay};
  }
  return {SafeState::Locked, Outcome::Wrong};  // недосяжно: стан поза enum
}

// Скільки стан чекає наступної події. 0 - без таймауту.
static constexpr uint32_t state_timeout_ms(SafeState state)
{
  switch (state) {
    case SafeState::Short1:
    case SafeState::Short2: return Config::INPUT_TIMEOUT_MS;
    case SafeState::Open:   return Config::OPEN_MS;
    case SafeState::Locked: return 0;
  }
  return 0;
}

// --- Перевірки під час збірки: послідовність подій -> кінцевий стан. ---
template <size_t N>
static constexpr SafeState feed(SafeState state, const Event (&events)[N])
{
  for (Event e : events) {
    state = safe_step(state, e).next;
  }
  return state;
}

namespace check {
using E = Event;
using S = SafeState;
constexpr E PASSWORD[]        = {E::Press, E::Short, E::Press, E::Short, E::Press, E::Long};
constexpr E BARE_PASSWORD[]   = {E::Short, E::Short, E::Long};
constexpr E LONG_FIRST[]      = {E::Long};
constexpr E LONG_SECOND[]     = {E::Short, E::Long};
constexpr E THREE_SHORT[]     = {E::Short, E::Short, E::Short};
constexpr E THREE_SHORT_LONG[] = {E::Short, E::Short, E::Short, E::Long};
constexpr E RETRY[]           = {E::Short, E::Long, E::Short, E::Short, E::Long};
constexpr E TIMEOUT_1[]       = {E::Short, E::Timeout};
constexpr E TIMEOUT_2[]       = {E::Short, E::Short, E::Timeout};
constexpr E TIMEOUT_THEN_LONG[] = {E::Short, E::Short, E::Timeout, E::Long};
constexpr E PRESS_ONLY[]      = {E::Press, E::Press, E::Press};
constexpr E CLOSE_SHORT[]     = {E::Short};
constexpr E CLOSE_TIMEOUT[]   = {E::Timeout};
constexpr E OPEN_LONG[]       = {E::Press, E::Long};

static_assert(feed(S::Locked, PASSWORD) == S::Open, "S-S-L with PRESS opens");
static_assert(feed(S::Locked, BARE_PASSWORD) == S::Open, "S-S-L opens");
static_assert(feed(S::Locked, LONG_FIRST) == S::Locked, "L first is wrong");
static_assert(feed(S::Locked, LONG_SECOND) == S::Locked, "S-L is wrong");
static_assert(feed(S::Locked, THREE_SHORT) == S::Locked, "third S is wrong");
static_assert(feed(S::Locked, THREE_SHORT_LONG) == S::Locked, "S-S-S-L must not open");
static_assert(feed(S::Locked, RETRY) == S::Open, "after a reset the password works again");
static_assert(feed(S::Locked, TIMEOUT_1) == S::Locked, "timeout after S resets");
static_assert(feed(S::Locked, TIMEOUT_2) == S::Locked, "timeout after S-S resets");
static_assert(feed(S::Locked, TIMEOUT_THEN_LONG) == S::Locked, "timeout loses S-S");
static_assert(feed(S::Locked, PRESS_ONLY) == S::Locked, "PRESS alone changes nothing");
static_assert(feed(S::Open, CLOSE_SHORT) == S::Locked, "S closes an open safe");
static_assert(feed(S::Open, CLOSE_TIMEOUT) == S::Locked, "open safe closes itself");
static_assert(feed(S::Open, OPEN_LONG) == S::Open, "L keeps the safe open");
static_assert(state_timeout_ms(S::Locked) == 0, "locked safe waits forever");
}  // namespace check

static const char *state_name(SafeState state)
{
  switch (state) {
    case SafeState::Locked: return "LOCKED";
    case SafeState::Short1: return "SHORT_1";
    case SafeState::Short2: return "SHORT_2";
    case SafeState::Open:   return "OPEN";
  }
  return "?";
}

static const char *event_name(Event event)
{
  switch (event) {
    case Event::Press:   return "PRESS";
    case Event::Short:   return "SHORT";
    case Event::Long:    return "LONG";
    case Event::Timeout: return "TIMEOUT";
  }
  return "?";
}

static void log_transition(const Message &msg, SafeState from, Transition t)
{
  const char *ev   = event_name(msg.event);
  const char *a    = state_name(from);
  const char *b    = state_name(t.next);
  uint32_t    time = msg.event == Event::Timeout ? state_timeout_ms(from) : msg.hold_ms;

  // PRESS не має тривалості: замість неї пробіли, щоб стовпці стояли рівно.
  char dur[16] = "       ";
  if (msg.event != Event::Press) {
    snprintf(dur, sizeof(dur), "%4" PRIu32 " ms", time);
  }

  switch (t.outcome) {
    case Outcome::Stay:
      log_line('I', "%-7s %s  %s%s", ev, dur, a,
               state_timeout_ms(from) > 0 ? ", timer restarted" : "");
      break;
    case Outcome::Step:
      log_line('I', "%-7s %s  %s -> %s", ev, dur, a, b);
      break;
    case Outcome::Unlock:
      log_line('I', "%-7s %s  %s -> %s, safe is open", ev, dur, a, b);
      break;
    case Outcome::Wrong:
      log_line('W', "%-7s %s  %s -> %s, wrong input", ev, dur, a, b);
      break;
    case Outcome::Expired:
      log_line('W', "%-7s %s  %s -> %s, input timeout", ev, dur, a, b);
      break;
    case Outcome::Close:
      log_line('I', "%-7s %s  %s -> %s, safe is closed", ev, dur, a, b);
      break;
  }
}

static void led_set(bool on)
{
  // Світлодіод плати підключений до 3V3 через резистор: горить, коли PC13 = 0.
  HAL_GPIO_WritePin(LED_PORT, Config::LED_PIN, on ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

static void safe_task(void *)
{
  SafeState  state      = SafeState::Locked;
  TickType_t last_event = xTaskGetTickCount();

  for (;;) {
    // Таймаут рахується від останньої події з черги, PRESS теж рахується. Тому автомат
    // не скинеться, поки користувач тримає кнопку і чекає на LONG.
    TickType_t wait  = portMAX_DELAY;
    uint32_t   limit = state_timeout_ms(state);
    if (limit > 0) {
      TickType_t passed = xTaskGetTickCount() - last_event;
      TickType_t total  = pdMS_TO_TICKS(limit);
      wait = passed >= total ? 0 : total - passed;
    }

    Message msg;
    if (xQueueReceive(queue, &msg, wait) == pdPASS) {
      last_event = xTaskGetTickCount();
    } else {
      msg = {Event::Timeout, 0};
    }

    Transition t = safe_step(state, msg.event);
    // Світлодіод - до логу: HAL_UART_Transmit займає кілька мс.
    if (t.next != state) {
      led_set(t.next == SafeState::Open);
    }
    log_transition(msg, state, t);
    state = t.next;
  }
}

// ---------------------------------------------------------------------------
// Старт: тактування -> GPIO -> UART -> черга -> задачі -> планувальник.
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

int main(void)
{
  HAL_Init();
  SystemClock_Config();
  gpio_init();
  uart_init();

  queue      = xQueueCreate(Config::QUEUE_LENGTH, sizeof(Message));
  uart_mutex = xSemaphoreCreateMutex();
  if (queue == nullptr || uart_mutex == nullptr) {
    Error_Handler();
  }

  // Планувальник ще не працює, тому рядок іде в UART напряму, без м'ютекса.
  static const char banner[] =
      "\r\n=== dz6.4 safe_fsm_stm32: password SHORT SHORT LONG (>= 1000 ms), "
      "timeout 3000 ms ===\r\n";
  HAL_UART_Transmit(&huart1, (uint8_t *)banner, sizeof banner - 1, HAL_MAX_DELAY);

  start(safe_task, "safe", Config::PRIO_SAFE);
  start(button_task, "button", Config::PRIO_BUTTON);

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
  // HSI 16 МГц без PLL, як у ДЗ 2.8, 2.9 і 6.1-6.3.
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
  __HAL_RCC_GPIOC_CLK_ENABLE();

  GPIO_InitTypeDef led = {};
  led.Pin   = Config::LED_PIN;
  led.Mode  = GPIO_MODE_OUTPUT_PP;
  led.Pull  = GPIO_NOPULL;
  led.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_WritePin(LED_PORT, Config::LED_PIN, GPIO_PIN_SET);  // вимкнений
  HAL_GPIO_Init(LED_PORT, &led);

  GPIO_InitTypeDef button = {};
  button.Pin  = Config::BUTTON_PIN;
  button.Mode = GPIO_MODE_INPUT;
  button.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(BUTTON_PORT, &button);
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
