// ДЗ 2.6, завдання 1 — базова реалізація без debounce.
// Порт raw_interrupt з ДЗ 2.4 на ESP-IDF: замість attachInterrupt() —
// gpio_install_isr_service(), замість опитування volatile у loop() —
// задача, що спить на нотифікації від ISR.

#include <atomic>
#include <inttypes.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

struct Config {
  static constexpr gpio_num_t BUTTON_PIN = GPIO_NUM_16;
  static constexpr gpio_num_t LED_PIN    = GPIO_NUM_4;
};

static const char *TAG = "raw_interrupt";

static TaskHandle_t s_worker;           // кому ISR віддає подію
static std::atomic<uint32_t> s_isr_count{0};   // скільки спадів побачив МК

// ISR робить рівно дві дії: інкремент лічильника і будильник задачі.
// Логування й gpio_set_level() лишаються поза перериванням.
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

// extern "C" обов'язковий: без нього ім'я спотворюється C++ і стартовий код
// ESP-IDF, написаний на C, точку входу не знаходить.
extern "C" void app_main()
{
    s_worker = xTaskGetCurrentTaskHandle();

    configure_gpio();
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(Config::BUTTON_PIN, button_isr, nullptr));

    ESP_LOGI(TAG, "ready, no debounce at all");

    uint32_t reported = 0;
    int led_level = 0;
    while (true) {
        // Нотифікації лічильні: поки задача друкувала, ISR міг нарахувати
        // ще кілька — тому реагуємо на лічильник, а не на кожне пробудження.
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
