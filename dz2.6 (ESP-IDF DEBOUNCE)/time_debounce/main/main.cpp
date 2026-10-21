// ДЗ 2.6, завдання 2 — software debounce по часу.
// Порт time_debounce з ДЗ 2.4 на ESP-IDF. Фільтр лишається поза ISR:
// обробник тільки знімає мітку часу і кладе її в чергу, рішення ухвалює задача.
// Замість micros() — esp_timer_get_time(): теж мікросекунди, теж в IRAM,
// але 64-бітний, тож через 71 хвилину не перевертається.

#include <atomic>
#include <inttypes.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

struct Config {
  static constexpr gpio_num_t BUTTON_PIN     = GPIO_NUM_16;
  static constexpr gpio_num_t LED_PIN        = GPIO_NUM_4;
  static constexpr uint32_t   DEBOUNCE_MS    = 50;
  static constexpr uint8_t    EDGE_QUEUE_LEN = 64;
};

static const char *TAG = "time_debounce";

static QueueHandle_t s_edges;           // мітки часу фронтів, ISR -> задача
static std::atomic<uint32_t> s_isr_count{0};   // скільки спадів побачив МК
static std::atomic<uint32_t> s_overruns{0};    // фронти, що не влізли в чергу

static void IRAM_ATTR button_isr(void *arg)
{
    const int64_t now = esp_timer_get_time();
    s_isr_count.fetch_add(1, std::memory_order_relaxed);

    // Той самий прапорець, що й у решти прошивок: без portYIELD_FROM_ISR
    // задача з черги прочитає подію лише на наступному такті планувальника.
    BaseType_t task_woken = pdFALSE;
    if (xQueueSendFromISR(s_edges, &now, &task_woken) != pdTRUE) {
        s_overruns.fetch_add(1, std::memory_order_relaxed);
    }
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

extern "C" void app_main()
{
    s_edges = xQueueCreate(Config::EDGE_QUEUE_LEN, sizeof(int64_t));
    ESP_ERROR_CHECK(s_edges != nullptr ? ESP_OK : ESP_ERR_NO_MEM);

    configure_gpio();
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(Config::BUTTON_PIN, button_isr, nullptr));

    ESP_LOGI(TAG, "ready, debounce window %" PRIu32 " ms", Config::DEBOUNCE_MS);

    int64_t last_accepted = 0;
    uint32_t accepted = 0;
    uint32_t overruns_seen = 0;
    int led_level = 0;

    while (true) {
        int64_t edge = 0;
        if (xQueueReceive(s_edges, &edge, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (edge - last_accepted >= static_cast<int64_t>(Config::DEBOUNCE_MS) * 1000) {
            last_accepted = edge;
            accepted++;
            led_level = !led_level;
            gpio_set_level(Config::LED_PIN, led_level);
            ESP_LOGI(TAG, "Accepted button events: %" PRIu32 ", total interrupts: %" PRIu32,
                     accepted, s_isr_count.load(std::memory_order_relaxed));
        } else {
            ESP_LOGI(TAG, "Rejected button event at: %" PRId64 " us", edge);
        }
        const uint32_t overruns = s_overruns.load(std::memory_order_relaxed);
        if (overruns != overruns_seen) {
            overruns_seen = overruns;
            ESP_LOGW(TAG, "Queue overruns: %" PRIu32, overruns_seen);
        }
    }
}
