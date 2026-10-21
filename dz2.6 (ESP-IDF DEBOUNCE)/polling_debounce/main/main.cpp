// ДЗ 2.6, завдання 4 — polling без переривань.
// Порт polling_debounce з ДЗ 2.4 на ESP-IDF. Переривань немає зовсім: окрема
// задача опитує пін раз на POLL_INTERVAL_MS, рівень стає справжнім тільки
// після STABLE_SAMPLES однакових вимірів поспіль.
//
// Такт відмірює xTaskDelayUntil() — аналог millis()-сітки з ДЗ 2.4, але без
// дрейфу: прив'язка йде до попереднього пробудження, а не до кінця обробки.

#include <inttypes.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

struct Config {
  static constexpr gpio_num_t BUTTON_PIN       = GPIO_NUM_16;
  static constexpr gpio_num_t LED_PIN          = GPIO_NUM_4;
  static constexpr int        PRESSED_LEVEL    = 0;
  static constexpr uint32_t   POLL_INTERVAL_MS = 5;
  static constexpr uint8_t    STABLE_SAMPLES   = 4;
};

// Текст асертів англійською свідомо: gcc друкує не-ASCII у повідомленні
// static_assert вісімковими escape-послідовностями, і українське
// формулювання в консолі перетворюється на кашу.
static_assert(Config::POLL_INTERVAL_MS >= 5 && Config::POLL_INTERVAL_MS <= 10,
              "task requires polling every 5-10 ms");
static_assert(1000u / CONFIG_FREERTOS_HZ <= Config::POLL_INTERVAL_MS,
              "scheduler tick is longer than the poll interval: set CONFIG_FREERTOS_HZ=1000");

static const char *TAG = "polling_debounce";

enum class ButtonState : uint8_t {
  Released,       // стабільно відпущена
  PressPending,   // побачили LOW, рахуємо підтвердження
  Pressed,        // стабільно натиснута
  ReleasePending  // побачили HIGH, рахуємо підтвердження
};

static void configure_gpio()
{
    const gpio_config_t button = {
        .pin_bit_mask = 1ULL << Config::BUTTON_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
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
    configure_gpio();
    ESP_LOGI(TAG, "ready, polling every %" PRIu32 " ms, %u samples to confirm",
             Config::POLL_INTERVAL_MS, Config::STABLE_SAMPLES);

    ButtonState state = ButtonState::Released;
    uint8_t stable_count = 0;
    uint32_t press_count = 0;
    int led_level = 0;

    TickType_t last_wake = xTaskGetTickCount();
    while (true) {
        xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(Config::POLL_INTERVAL_MS));

        const bool level_pressed = gpio_get_level(Config::BUTTON_PIN) == Config::PRESSED_LEVEL;
        switch (state) {
        case ButtonState::Released:
            if (level_pressed) {
                state = ButtonState::PressPending;
                stable_count = 1;
            }
            break;
        case ButtonState::PressPending:
            if (!level_pressed) {
                state = ButtonState::Released;
            } else if (++stable_count >= Config::STABLE_SAMPLES) {
                state = ButtonState::Pressed;
                press_count++;
                led_level = !led_level;
                gpio_set_level(Config::LED_PIN, led_level);
                ESP_LOGI(TAG, "Button pressed: %" PRIu32, press_count);
            }
            break;
        case ButtonState::Pressed:
            if (!level_pressed) {
                state = ButtonState::ReleasePending;
                stable_count = 1;
            }
            break;
        case ButtonState::ReleasePending:
            if (level_pressed) {
                state = ButtonState::Pressed;
            } else if (++stable_count >= Config::STABLE_SAMPLES) {
                state = ButtonState::Released;
                ESP_LOGI(TAG, "Button released");
            }
            break;
        }
    }
}
