// ДЗ 2.6, завдання 3 — debounce по рівню.
// Порт state_debounce з ДЗ 2.4 на ESP-IDF. Переривання на обидва фронти лише
// повідомляє «на лінії щось сталося», задача витримує SETTLE_MS і читає
// справжній рівень піна.
//
// Замість delay(10) тут vTaskDelay(), і це не косметика: delay() в Arduino
// крутить цикл, а vTaskDelay() віддає процесор. Ціна — такт планувальника:
// за замовчуванням ESP-IDF має CONFIG_FREERTOS_HZ=100, тобто 10 мс
// квантування. Тому в sdkconfig.defaults стоїть 1000 Гц.

#include <atomic>
#include <inttypes.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

struct Config {
  static constexpr gpio_num_t BUTTON_PIN    = GPIO_NUM_16;
  static constexpr gpio_num_t LED_PIN       = GPIO_NUM_4;
  static constexpr int        PRESSED_LEVEL = 0;
  static constexpr uint32_t   SETTLE_MS     = 10;
};

// Англійською свідомо: gcc друкує не-ASCII у повідомленні static_assert
// вісімковими escape-послідовностями, і український текст стає кашею.
static_assert(1000u / CONFIG_FREERTOS_HZ <= Config::SETTLE_MS,
              "scheduler tick is longer than the settle time: set CONFIG_FREERTOS_HZ=1000");

static const char *TAG = "state_debounce";

static TaskHandle_t s_worker;
static std::atomic<uint32_t> s_isr_count{0};

static void IRAM_ATTR button_isr(void *arg)
{
    s_isr_count.fetch_add(1, std::memory_order_relaxed);

    // pxHigherPriorityTaskWoken + portYIELD_FROM_ISR обов'язкові: без них
    // розбуджена задача чекає наступного такту планувальника, а він тут
    // 100 Гц, тож реакція поїхала б на випадкові 0-10 мс.
    BaseType_t task_woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_worker, &task_woken);
    portYIELD_FROM_ISR(task_woken);
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
    s_worker = xTaskGetCurrentTaskHandle();

    configure_gpio();
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(Config::BUTTON_PIN, button_isr, nullptr));

    ESP_LOGI(TAG, "ready, settle time %" PRIu32 " ms, tick %d Hz",
             Config::SETTLE_MS, CONFIG_FREERTOS_HZ);

    bool pressed = false;
    uint32_t press_count = 0;
    int led_level = 0;

    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(Config::SETTLE_MS));

        const bool level_pressed = gpio_get_level(Config::BUTTON_PIN) == Config::PRESSED_LEVEL;
        if (level_pressed && !pressed) {
            pressed = true;
            press_count++;
            led_level = !led_level;
            gpio_set_level(Config::LED_PIN, led_level);
            ESP_LOGI(TAG, "Button pressed: %" PRIu32 ", total interrupts: %" PRIu32,
                     press_count, s_isr_count.load(std::memory_order_relaxed));
        } else if (!level_pressed && pressed) {
            pressed = false;
            ESP_LOGI(TAG, "Button released");
        }
    }
}
