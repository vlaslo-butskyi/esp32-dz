// ДЗ 2.6, завдання 5 — debounce на одноразовому таймері esp_timer.
// Того, що тут робиться, в ДЗ 2.4 не було: витримку відмірює не затримка в
// задачі, а апаратний таймер, і кожен новий фронт її перезапускає.
//
// ISR не чекає нічого: знімає лічильник, глушить попередній відлік і заводить
// esp_timer на SETTLE_MS. Поки контакт брязкотить, таймер перезаводиться і
// ніколи не доходить до кінця. Коли лінія затихла на SETTLE_MS — колбек читає
// рівень піна і вирішує, що це було.
//
// esp_timer_stop() і esp_timer_start_once() помічені ESP_TIMER_IRAM_ATTR і
// беруть спінлок через portENTER_CRITICAL_SAFE, тож викликати їх із
// переривання можна. Колбек натомість виконується в задачі esp_timer,
// спільній для всіх таймерів системи, тому в ньому тільки світлодіод і
// черга — лог друкує окрема задача, інакше повільний UART тримав би всю
// чергу таймерів.

#include <atomic>
#include <inttypes.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

struct Config {
  static constexpr gpio_num_t BUTTON_PIN      = GPIO_NUM_16;
  static constexpr gpio_num_t LED_PIN         = GPIO_NUM_4;
  static constexpr int        PRESSED_LEVEL   = 0;
  static constexpr uint32_t   SETTLE_MS       = 10;
  static constexpr uint8_t    EVENT_QUEUE_LEN = 16;
};

static const char *TAG = "timer_debounce";

struct ButtonEvent {
  bool pressed;
  uint32_t press_count;
  uint32_t isr_count;
  uint32_t restarts;
};

static esp_timer_handle_t s_settle_timer;
static QueueHandle_t s_events;           // колбек таймера -> задача-репортер

static std::atomic<uint32_t> s_isr_count{0};    // фронтів побачив МК
static std::atomic<uint32_t> s_restarts{0};     // скільки з них перезавели таймер

static void IRAM_ATTR button_isr(void *arg)
{
    s_isr_count.fetch_add(1, std::memory_order_relaxed);
    // Перший фронт заводить таймер, кожен наступний — перезаводить.
    // ESP_ERR_INVALID_STATE означає «таймер і не був запущений», це норма.
    if (esp_timer_stop(s_settle_timer) == ESP_OK) {
        s_restarts.fetch_add(1, std::memory_order_relaxed);
    }
    esp_timer_start_once(s_settle_timer, Config::SETTLE_MS * 1000ULL);
}

// Лінія мовчала SETTLE_MS — тепер рівень піна вже щось означає.
static void settle_timer_cb(void *arg)
{
    static bool pressed = false;
    static uint32_t press_count = 0;
    static int led_level = 0;

    const bool level_pressed = gpio_get_level(Config::BUTTON_PIN) == Config::PRESSED_LEVEL;
    if (level_pressed == pressed) {
        return;  // брязкіт закінчився там, де й почався
    }

    pressed = level_pressed;
    if (pressed) {
        press_count++;
        led_level = !led_level;
        gpio_set_level(Config::LED_PIN, led_level);
    }

    const ButtonEvent event = {
        .pressed = pressed,
        .press_count = press_count,
        .isr_count = s_isr_count.load(std::memory_order_relaxed),
        .restarts = s_restarts.load(std::memory_order_relaxed),
    };
    xQueueSend(s_events, &event, 0);
}

static void configure_gpio()
{
    const gpio_config_t button = {
        .pin_bit_mask = 1ULL << Config::BUTTON_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&button));

    const gpio_config_t led = {
        .pin_bit_mask = 1ULL << Config::LED_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&led));
    ESP_ERROR_CHECK(gpio_set_level(Config::LED_PIN, 0));
}

extern "C" void app_main()
{
    s_events = xQueueCreate(Config::EVENT_QUEUE_LEN, sizeof(ButtonEvent));
    ESP_ERROR_CHECK(s_events != nullptr ? ESP_OK : ESP_ERR_NO_MEM);

    // Поля перелічені всі й у порядку оголошення: C пропущені члени прощав,
    // а C++ під -Wextra дає -Wmissing-field-initializers.
    const esp_timer_create_args_t settle_args = {
        .callback = settle_timer_cb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "settle",
        .skip_unhandled_events = false,
    };
    ESP_ERROR_CHECK(esp_timer_create(&settle_args, &s_settle_timer));

    configure_gpio();
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(Config::BUTTON_PIN, button_isr, nullptr));

    ESP_LOGI(TAG, "ready, one-shot esp_timer settle %" PRIu32 " ms, retriggered on every edge",
             Config::SETTLE_MS);

    while (true) {
        ButtonEvent event;
        if (xQueueReceive(s_events, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (event.pressed) {
            ESP_LOGI(TAG, "Button pressed: %" PRIu32 ", interrupts: %" PRIu32
                          ", timer restarts: %" PRIu32,
                     event.press_count, event.isr_count, event.restarts);
        } else {
            ESP_LOGI(TAG, "Button released");
        }
    }
}
