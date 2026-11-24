// ДЗ 6.4: електронний сейф на кінцевому автоматі. Пароль: Коротке - Коротке - Довге.
//
//   button (задача, Producer) - автомат кнопки: опитує GPIO 16 кожні 10 мс, придушує брязкіт
//                               і кладе в чергу події PRESS, SHORT, LONG;
//   safe   (задача, Consumer) - автомат сейфа: читає чергу і перемикає стан сейфа.
//
// Автомати не знають один про одного: між ними тільки черга подій.

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "safe";

struct Config {
  static constexpr gpio_num_t BUTTON_PIN = GPIO_NUM_16;  // другий контакт на GND, натиснута = 0
  static constexpr gpio_num_t LED_PIN    = GPIO_NUM_4;   // світиться, поки сейф відкритий

  static constexpr uint32_t POLL_MS     = 10;    // період опитування кнопки
  static constexpr uint32_t DEBOUNCE_MS = 30;    // стільки рівень має не змінюватися
  static constexpr uint32_t LONG_MS     = 1000;  // утримання від цього часу = LONG

  static constexpr uint32_t INPUT_TIMEOUT_MS = 3000;  // «затупив»: тиша між подіями
  static constexpr uint32_t OPEN_MS          = 5000;  // сейф сам закривається

  static constexpr UBaseType_t QUEUE_LENGTH  = 8;
  static constexpr UBaseType_t PRIO_BUTTON   = 5;
  static constexpr UBaseType_t PRIO_SAFE     = 4;
  static constexpr BaseType_t  CORE          = 1;
  static constexpr uint32_t    STACK         = 3072;
};

static_assert(Config::DEBOUNCE_MS >= 2 * Config::POLL_MS, "debounce needs at least 2 polls");
static_assert(Config::LONG_MS > Config::DEBOUNCE_MS, "a long press must outlast debounce");
static_assert(Config::INPUT_TIMEOUT_MS > Config::LONG_MS,
              "LONG must arrive before the input timeout, counted from PRESS");

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
  return pdTICKS_TO_MS(ticks);
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
    ESP_LOGW(TAG, "queue full, event dropped");
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
    bool raw = gpio_get_level(Config::BUTTON_PIN) == 0;
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
      ESP_LOGI(TAG, "%-7s %s  %s%s", ev, dur, a,
               state_timeout_ms(from) > 0 ? ", timer restarted" : "");
      break;
    case Outcome::Step:
      ESP_LOGI(TAG, "%-7s %s  %s -> %s", ev, dur, a, b);
      break;
    case Outcome::Unlock:
      ESP_LOGI(TAG, "%-7s %s  %s -> %s, safe is open", ev, dur, a, b);
      break;
    case Outcome::Wrong:
      ESP_LOGW(TAG, "%-7s %s  %s -> %s, wrong input", ev, dur, a, b);
      break;
    case Outcome::Expired:
      ESP_LOGW(TAG, "%-7s %s  %s -> %s, input timeout", ev, dur, a, b);
      break;
    case Outcome::Close:
      ESP_LOGI(TAG, "%-7s %s  %s -> %s, safe is closed", ev, dur, a, b);
      break;
  }
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
    log_transition(msg, state, t);

    if (t.next != state) {
      gpio_set_level(Config::LED_PIN, t.next == SafeState::Open ? 1 : 0);
      state = t.next;
    }
  }
}

// ---------------------------------------------------------------------------
// Старт: GPIO -> черга -> consumer -> producer.
// ---------------------------------------------------------------------------
static void gpio_init(void)
{
  gpio_config_t button = {};
  button.pin_bit_mask = 1ULL << Config::BUTTON_PIN;
  button.mode         = GPIO_MODE_INPUT;
  button.pull_up_en   = GPIO_PULLUP_ENABLE;
  ESP_ERROR_CHECK(gpio_config(&button));

  gpio_config_t led = {};
  led.pin_bit_mask = 1ULL << Config::LED_PIN;
  led.mode         = GPIO_MODE_OUTPUT;
  ESP_ERROR_CHECK(gpio_config(&led));
  ESP_ERROR_CHECK(gpio_set_level(Config::LED_PIN, 0));
}

static void start(TaskFunction_t fn, const char *name, UBaseType_t prio)
{
  if (xTaskCreatePinnedToCore(fn, name, Config::STACK, nullptr, prio, nullptr, Config::CORE) !=
      pdPASS) {
    ESP_LOGE(TAG, "no memory for task %s", name);
    abort();
  }
}

extern "C" void app_main(void)
{
  gpio_init();

  queue = xQueueCreate(Config::QUEUE_LENGTH, sizeof(Message));
  if (queue == nullptr) {
    ESP_LOGE(TAG, "no memory for queue");
    abort();
  }

  start(safe_task, "safe", Config::PRIO_SAFE);
  start(button_task, "button", Config::PRIO_BUTTON);

  ESP_LOGI(TAG, "password: SHORT SHORT LONG (>= %" PRIu32 " ms), timeout %" PRIu32 " ms",
           Config::LONG_MS, Config::INPUT_TIMEOUT_MS);
}
