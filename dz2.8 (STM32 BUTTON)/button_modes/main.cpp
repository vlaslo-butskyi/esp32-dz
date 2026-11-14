// ДЗ 2.8 — STM32: обробка кнопки, реакція світлодіода.
//
// Код з уроку був такий:
//
//     HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin);
//     HAL_Delay(500);
//
// Додати до нього опитування кнопки «в лоб» не вийде: HAL_Delay() — це
// порожній цикл очікування, і поки він крутиться, читати пін нема кому.
// Натискання, яке почалось і закінчилось усередині цих 500 мс, прошивка
// просто не побачить. Тому замість HAL_Delay() тут сітка на HAL_GetTick() —
// той самий прийом, що millis() у ДЗ 2.3, — і цикл більше ніде не блокується.
//
// Кнопка перемикає режими по колу: вимкнено -> повільно -> швидко -> рівно.
// Брязкіт гасить витримка по рівню: рівень має протриматися незмінним
// DEBOUNCE_MS, і лише тоді вважається станом кнопки (метод state_debounce
// із ДЗ 2.4).

#include "stm32f4xx_hal.h"

// У CubeMX ці імена генеруються з міток пінів. Збірка тут без генератора,
// тож визначаю їх сам — щоб код з уроку лишився дослівно впізнаваним.
#define LED_GPIO_Port    GPIOC
#define LED_Pin          GPIO_PIN_13
#define BUTTON_GPIO_Port GPIOA
#define BUTTON_Pin       GPIO_PIN_0

struct Config {
  // Світлодіод на Black Pill сидить катодом на PC13, анодом через резистор на
  // 3,3 В — тобто горить від НИЗЬКОГО рівня, а не від високого.
  static constexpr GPIO_PinState LED_ON  = GPIO_PIN_RESET;
  static constexpr GPIO_PinState LED_OFF = GPIO_PIN_SET;

  // Кнопка KEY замикає PA0 на землю; підтяжка внутрішня, тож натиснута — нуль.
  static constexpr GPIO_PinState BUTTON_PRESSED = GPIO_PIN_RESET;

  static constexpr uint32_t SLOW_MS     = 500;  // період із коду уроку
  static constexpr uint32_t FAST_MS     = 100;
  static constexpr uint32_t DEBOUNCE_MS = 20;
};

enum class Mode : uint8_t { Off, Slow, Fast, Solid };
static constexpr uint8_t MODE_COUNT = 4;

struct ModeDef {
  Mode     self;      // дублює індекс, щоб перестановку рядків спіймав компілятор
  uint32_t blink_ms;  // 0 — не блимає
  bool     lit;       // стан світлодіода, коли не блимає
};

static constexpr ModeDef MODES[MODE_COUNT] = {
  { Mode::Off,   0,                false },
  { Mode::Slow,  Config::SLOW_MS,  false },
  { Mode::Fast,  Config::FAST_MS,  false },
  { Mode::Solid, 0,                true  },
};

constexpr const ModeDef &def_of(Mode m) { return MODES[static_cast<uint8_t>(m)]; }

constexpr bool table_matches_enum()
{
    for (uint8_t i = 0; i < MODE_COUNT; i++) {
        if (static_cast<uint8_t>(MODES[i].self) != i) return false;
    }
    return true;
}
static_assert(table_matches_enum(), "MODES rows must stay in the order of enum class Mode");
static_assert(Config::FAST_MS < Config::SLOW_MS, "fast mode must blink faster than slow");
static_assert(Config::DEBOUNCE_MS < Config::FAST_MS,
              "debounce must be shorter than the fastest blink, otherwise a press could be "
              "swallowed between two toggles");

// ─────────────────────────── ініціалізація ───────────────────────────

static void Error_Handler()
{
    __disable_irq();
    while (true) {
    }
}

static void SystemClock_Config()
{
    // Тактування від внутрішнього HSI 16 МГц, без PLL. Для блимання й кнопки
    // більше не потрібно, зате ця конфігурація однакова для F401 і F411: межі
    // PLL у них різні (84 і 100 МГц), спільного набору дільників немає.
    RCC_OscInitTypeDef osc = {};
    osc.OscillatorType      = RCC_OSCILLATORTYPE_HSI;
    osc.HSIState            = RCC_HSI_ON;
    osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    osc.PLL.PLLState        = RCC_PLL_NONE;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
        Error_Handler();
    }

    RCC_ClkInitTypeDef clk = {};
    clk.ClockType      = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_PCLK1 |
                         RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource   = RCC_SYSCLKSOURCE_HSI;
    clk.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV1;
    clk.APB2CLKDivider = RCC_HCLK_DIV1;
    // На 16 МГц флеш читається без тактів очікування.
    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_0) != HAL_OK) {
        Error_Handler();
    }
}

static void gpio_init()
{
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    // Світлодіод гасимо ДО переведення піна у вихід: інакше між перемиканням
    // режиму піна і першим записом він коротко спалахнув би.
    HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, Config::LED_OFF);

    GPIO_InitTypeDef led = {};
    led.Pin   = LED_Pin;
    led.Mode  = GPIO_MODE_OUTPUT_PP;
    led.Pull  = GPIO_NOPULL;
    led.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(LED_GPIO_Port, &led);

    GPIO_InitTypeDef button = {};
    button.Pin  = BUTTON_Pin;
    button.Mode = GPIO_MODE_INPUT;
    button.Pull = GPIO_PULLUP;  // кнопка замикає на землю, зовнішньої підтяжки немає
    HAL_GPIO_Init(BUTTON_GPIO_Port, &button);
}

// PlatformIO з framework = stm32cube не кладе згенерований stm32f4xx_it.c,
// тому обробник системного таймера треба оголосити самому. Без нього
// HAL_GetTick() назавжди лишається нулем, а HAL_Delay() не повертається.
// extern "C" обов'язковий: ім'я має збігтися з тим, що в таблиці векторів,
// а C++ без цієї обгортки його спотворив би.
extern "C" void SysTick_Handler(void)
{
    HAL_IncTick();
}

// ─────────────────────────── прошивка ───────────────────────────

static void led_write(bool on)
{
    HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, on ? Config::LED_ON : Config::LED_OFF);
}

static Mode next_mode(Mode m)
{
    return static_cast<Mode>((static_cast<uint8_t>(m) + 1) % MODE_COUNT);
}

int main()
{
    HAL_Init();
    SystemClock_Config();
    gpio_init();

    Mode          mode        = Mode::Off;
    bool          led_on      = def_of(mode).lit;
    uint32_t      last_toggle = 0;
    GPIO_PinState last_level  = GPIO_PIN_SET;  // кнопка не натиснута
    uint32_t      last_edge   = 0;
    bool          pressed     = false;

    led_write(led_on);

    while (true) {
        // Усі порівняння часу — різницею беззнакових, а не `now >= deadline`.
        // HAL_GetTick() перевертається через 49,7 доби, і тільки різниця
        // переживає це перевертання правильно.
        const uint32_t now = HAL_GetTick();

        // ── кнопка: витримка по рівню ──
        const GPIO_PinState level = HAL_GPIO_ReadPin(BUTTON_GPIO_Port, BUTTON_Pin);
        if (level != last_level) {
            last_level = level;
            last_edge  = now;  // лінія смикнулась — відлік тиші спочатку
        } else if (now - last_edge >= Config::DEBOUNCE_MS) {
            const bool stable_pressed = (level == Config::BUTTON_PRESSED);
            if (stable_pressed != pressed) {
                pressed = stable_pressed;
                if (pressed) {  // режим перемикає натискання, не відпускання
                    mode        = next_mode(mode);
                    led_on      = def_of(mode).lit;
                    last_toggle = now;
                    led_write(led_on);
                }
            }
        }

        // ── світлодіод ──
        const uint32_t blink_ms = def_of(mode).blink_ms;
        if (blink_ms != 0 && now - last_toggle >= blink_ms) {
            // Якір на сітку, а не на now: інакше час проходу циклу щоразу
            // додавався б до періоду і блимання повзло б.
            last_toggle += blink_ms;
            led_on = !led_on;
            led_write(led_on);
        }
    }
}
