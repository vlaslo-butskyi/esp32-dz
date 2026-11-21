#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "tasks";

struct Config {
  static constexpr gpio_num_t LED_RED_PIN    = GPIO_NUM_4;
  static constexpr gpio_num_t LED_GREEN_PIN  = GPIO_NUM_5;
  static constexpr gpio_num_t LED_YELLOW_PIN = GPIO_NUM_6;

  static constexpr uint32_t LED_RED_INTERVAL_MS    = 200;
  static constexpr uint32_t LED_GREEN_INTERVAL_MS  = 500;
  static constexpr uint32_t LED_YELLOW_INTERVAL_MS = 1000;

  static constexpr UBaseType_t LED_PRIORITY = 3;
  static constexpr BaseType_t  LED_CORE     = 1;
  static constexpr uint32_t    LED_STACK    = 2048;
};

struct Blinker {
  const char  *name;
  gpio_num_t  pin;
  uint32_t    interval_ms;
};

static const Blinker LEDS[] = {
  { 
    "red",    
    Config::LED_RED_PIN,    
    Config::LED_RED_INTERVAL_MS    
  },
  { 
    "green",  
    Config::LED_GREEN_PIN,  
    Config::LED_GREEN_INTERVAL_MS  
  },
  { 
    "yellow", 
    Config::LED_YELLOW_PIN, 
    Config::LED_YELLOW_INTERVAL_MS 
  }
};

static void blink_task(void *arg)
{
  const Blinker &led = *static_cast<const Blinker *>(arg);

  gpio_reset_pin(led.pin);
  gpio_set_direction(led.pin, GPIO_MODE_OUTPUT);
  bool level = false;
  gpio_set_level(led.pin, level);

  TickType_t last_wake = xTaskGetTickCount();
  for (;;) {
    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(led.interval_ms));

    level = !level;
    gpio_set_level(led.pin, level);
  }
}

extern "C" void app_main(void)
{
  for (const Blinker &led : LEDS) {
    xTaskCreatePinnedToCore(
      blink_task,
      led.name, 
      Config::LED_STACK,
      (void *)&led,
      Config::LED_PRIORITY,
      nullptr,
      Config::LED_CORE
    );
  }

  ESP_LOGI(TAG, "3 blink tasks started on core %d", int(Config::LED_CORE));
}
