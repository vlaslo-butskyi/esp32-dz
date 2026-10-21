// ДЗ 2.6, завдання 6 — апаратний glitch filter ESP32-S3.
// Код той самий, що в raw_interrupt: ISR рахує спади, фільтра в програмі немає.
// Різниця в трьох рядках configure_glitch_filter() — вхід GPIO проходить через
// апаратний фільтр, який живе в IO MUX, а не в коді.
//
// ESP32-S3 має лише "pin" glitch filter із зашитими параметрами:
// SOC_GPIO_SUPPORT_PIN_GLITCH_FILTER = 1, а SOC_GPIO_FLEX_GLITCH_FILTER_NUM
// не визначений узагалі. Налаштувати вікно не можна: ріжеться все, що коротше
// за два такти джерела IO MUX. Джерело на S3 одне — APB, 80 МГц, тобто поріг
// приблизно 25 нс.
//
// Для порівняння: найкоротша голка, зловлена аналізатором у ДЗ 2.4, була 41 нс,
// а найдовша пачка брязкоту — 0.8 мс. Тобто цей фільтр і не призначений для
// кнопки, він проти наведень на лінії; у ДЗ 2.4 цю роль виконував конденсатор.

#include <atomic>
#include <inttypes.h>

#include "driver/gpio.h"
#include "driver/gpio_filter.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

struct Config {
  static constexpr gpio_num_t BUTTON_PIN = GPIO_NUM_16;
  static constexpr gpio_num_t LED_PIN    = GPIO_NUM_4;
};

static const char *TAG = "glitch_filter";

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
        .intr_type = GPIO_INTR_NEGEDGE,
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

static void configure_glitch_filter()
{
    gpio_glitch_filter_handle_t filter = nullptr;
    const gpio_pin_glitch_filter_config_t config = {
        .clk_src = GLITCH_FILTER_CLK_SRC_DEFAULT,  // на ESP32-S3 це APB, 80 МГц
        .gpio_num = Config::BUTTON_PIN,
    };
    ESP_ERROR_CHECK(gpio_new_pin_glitch_filter(&config, &filter));
    ESP_ERROR_CHECK(gpio_glitch_filter_enable(filter));
}

extern "C" void app_main()
{
    s_worker = xTaskGetCurrentTaskHandle();

    configure_gpio();
    configure_glitch_filter();
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(Config::BUTTON_PIN, button_isr, nullptr));

    ESP_LOGI(TAG, "ready, hardware pin glitch filter on GPIO %d, no software debounce",
             Config::BUTTON_PIN);

    uint32_t reported = 0;
    int led_level = 0;
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const uint32_t count = s_isr_count.load(std::memory_order_relaxed);
        if (count == reported) {
            continue;
        }
        reported = count;
        led_level = !led_level;
        gpio_set_level(Config::LED_PIN, led_level);
        ESP_LOGI(TAG, "Button interrupt count: %" PRIu32, count);
    }
}
