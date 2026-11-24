// ДЗ 6.4 на ESP32-S3: електронний сейф на кінцевому автоматі. Пароль: Коротке - Коротке - Довге.
//
//   button (задача, Producer) - опитує GPIO 16 кожні 10 мс і передає рівень автомату кнопки;
//   safe   (задача, Consumer) - читає чергу, передає подію автомату сейфа, керує світлодіодом.
//
// Обидва автомати лежать у ../common і не знають ні про FreeRTOS, ні про GPIO.
// Тут тільки периферія, черга, задачі й лог.

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "button_fsm.h"
#include "safe_fsm.h"

static const char *TAG = "safe";

static const gpio_num_t BUTTON_PIN = GPIO_NUM_16;  // другий контакт на GND, натиснута = 0
static const gpio_num_t LED_PIN    = GPIO_NUM_4;   // світиться, поки сейф відкритий

enum {
  QUEUE_LENGTH = 8,
  PRIO_BUTTON  = 5,  // опитування не чекає на сейф
  PRIO_SAFE    = 4,
  CORE         = 1,
  STACK        = 3072,
};

_Static_assert(PRIO_BUTTON > PRIO_SAFE, "button polling must not wait for the safe");

static QueueHandle_t queue;

static uint32_t now_ms(void)
{
  return pdTICKS_TO_MS(xTaskGetTickCount());
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
    ESP_LOGW(TAG, "queue full, %s dropped", event_name(msg->event));
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
    bool pressed = gpio_get_level(BUTTON_PIN) == 0;
    button_poll(&button, pressed, now_ms(), queue_send, NULL);
  }
}

// ---------------------------------------------------------------------------
// Consumer
// ---------------------------------------------------------------------------
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
    if (t.next != from) {
      gpio_set_level(LED_PIN, t.next == SAFE_OPEN);
    }

    char line[96];
    if (safe_describe(line, sizeof line, &msg, from, t) == 'W') {
      ESP_LOGW(TAG, "%s", line);
    } else {
      ESP_LOGI(TAG, "%s", line);
    }
  }
}

// ---------------------------------------------------------------------------
// Старт: GPIO -> черга -> consumer -> producer.
// ---------------------------------------------------------------------------
static void gpio_init(void)
{
  gpio_config_t button = {
    .pin_bit_mask = 1ULL << BUTTON_PIN,
    .mode         = GPIO_MODE_INPUT,
    .pull_up_en   = GPIO_PULLUP_ENABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type    = GPIO_INTR_DISABLE,
  };
  ESP_ERROR_CHECK(gpio_config(&button));

  gpio_config_t led = {
    .pin_bit_mask = 1ULL << LED_PIN,
    .mode         = GPIO_MODE_OUTPUT,
    .pull_up_en   = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type    = GPIO_INTR_DISABLE,
  };
  ESP_ERROR_CHECK(gpio_config(&led));
  ESP_ERROR_CHECK(gpio_set_level(LED_PIN, 0));
}

static void start(TaskFunction_t fn, const char *name, UBaseType_t prio)
{
  if (xTaskCreatePinnedToCore(fn, name, STACK, NULL, prio, NULL, CORE) != pdPASS) {
    ESP_LOGE(TAG, "no memory for task %s", name);
    abort();
  }
}

void app_main(void)
{
  gpio_init();

  queue = xQueueCreate(QUEUE_LENGTH, sizeof(event_msg_t));
  if (queue == NULL) {
    ESP_LOGE(TAG, "no memory for queue");
    abort();
  }

  start(safe_task, "safe", PRIO_SAFE);
  start(button_task, "button", PRIO_BUTTON);

  ESP_LOGI(TAG, "password: SHORT SHORT LONG (>= %d ms), timeout %d ms", BUTTON_LONG_MS,
           SAFE_INPUT_TIMEOUT_MS);
}
