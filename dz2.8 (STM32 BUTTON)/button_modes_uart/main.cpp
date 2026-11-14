// ДЗ 2.8 — button_modes_uart: та сама прошивка, що button_modes, плюс лог у UART.
//
// Це діагностичний варіант, а не сама домашка: button_modes лишається без змін.
// Знадобився він тому, що подивитися на поведінку кнопки в цій звʼязці нема де.
// Послідовного порту тут немає взагалі: USB-C плати заведений на піни USB
// мікроконтролера (PA11/PA12), а не на мікросхему USB-UART, як на ESP32-S3
// DevKit, а синій ST-LINK V2 не має Virtual COM Port — він є тільки у V2-1 і V3
// на платах Nucleo. Тому лог іде в USART1 на PA9, а читати його можна або
// USB-UART перехідником, або логічним аналізатором із декодером Async Serial.
//
// Головне обмеження: лог не має права блокувати цикл. Уся суть button_modes у
// тому, що цикл ніде не чекає. HAL_UART_Transmit() чекав би на кожен рядок по
// кілька мілісекунд і зіпсував би саме те, що міряє: витримка debounce тут
// 20 мс, і трьох рядків досить, щоб натискання загубилося між опитуваннями.
// Тому рядки складаються в кільцевий буфер, а в UART їх зливає по одному байту
// log_pump() — рівно тоді, коли передавач вільний, і ніколи його не чекаючи.

#include "stm32f4xx_hal.h"

#define LED_GPIO_Port    GPIOC
#define LED_Pin          GPIO_PIN_13
#define BUTTON_GPIO_Port GPIOA
#define BUTTON_Pin       GPIO_PIN_0
#define LOG_GPIO_Port    GPIOA
#define LOG_TX_Pin       GPIO_PIN_9  // USART1_TX, альтернативна функція AF7

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

  static constexpr uint32_t LOG_BAUD = 115200;
};

enum class Mode : uint8_t { Off, Slow, Fast, Solid };
static constexpr uint8_t MODE_COUNT = 4;

struct ModeDef {
  Mode        self;      // дублює індекс, щоб перестановку рядків спіймав компілятор
  uint32_t    blink_ms;  // 0 — не блимає
  bool        lit;       // стан світлодіода, коли не блимає
  const char *name;      // лише для логу
};

static constexpr ModeDef MODES[MODE_COUNT] = {
  { Mode::Off,   0,                false, "Off"   },
  { Mode::Slow,  Config::SLOW_MS,  false, "Slow"  },
  { Mode::Fast,  Config::FAST_MS,  false, "Fast"  },
  { Mode::Solid, 0,                true,  "Solid" },
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
    // Тактування від внутрішнього HSI 16 МГц, без PLL — як у button_modes.
    // Для UART це значить, що точність бодрейту тримається на точності HSI
    // (±1 % із заводським калібруванням). 115200 з 16 МГц ділиться з похибкою
    // 0,08 %, тож запас на HSI лишається весь.
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

// ─────────────────────────── лог у USART1 ───────────────────────────

static UART_HandleTypeDef huart1;

static void uart_init()
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_USART1_CLK_ENABLE();

    GPIO_InitTypeDef tx = {};
    tx.Pin       = LOG_TX_Pin;
    tx.Mode      = GPIO_MODE_AF_PP;
    tx.Pull      = GPIO_NOPULL;
    tx.Speed     = GPIO_SPEED_FREQ_HIGH;
    tx.Alternate = GPIO_AF7_USART1;
    HAL_GPIO_Init(LOG_GPIO_Port, &tx);

    huart1.Instance          = USART1;
    huart1.Init.BaudRate     = Config::LOG_BAUD;
    huart1.Init.WordLength   = UART_WORDLENGTH_8B;
    huart1.Init.StopBits     = UART_STOPBITS_1;
    huart1.Init.Parity       = UART_PARITY_NONE;
    huart1.Init.Mode         = UART_MODE_TX;  // приймати тут нема чого
    huart1.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
    huart1.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&huart1) != HAL_OK) {
        Error_Handler();
    }
}

// Степінь двійки, щоб обгортання індексу було маскою, а не діленням.
static constexpr uint16_t LOG_BUF = 512;
static_assert((LOG_BUF & (LOG_BUF - 1)) == 0, "LOG_BUF must be a power of two");

static char     log_buf[LOG_BUF];
static uint16_t log_head    = 0;
static uint16_t log_tail    = 0;
static uint32_t log_dropped = 0;

static void log_putc(char c)
{
    const uint16_t next = (log_head + 1) & (LOG_BUF - 1);
    if (next == log_tail) {  // буфер повний — краще втратити рядок, ніж такт циклу
        log_dropped++;
        return;
    }
    log_buf[log_head] = c;
    log_head          = next;
}

static void log_puts(const char *s)
{
    while (*s != '\0') {
        log_putc(*s++);
    }
}

static void log_u32(uint32_t v, uint8_t width = 0)
{
    char    digits[10];
    uint8_t n = 0;
    do {
        digits[n++] = static_cast<char>('0' + v % 10);
        v /= 10;
    } while (v != 0);

    for (uint8_t i = n; i < width; i++) {
        log_putc(' ');  // праве вирівнювання, щоб стовпчик часу не стрибав
    }
    while (n != 0) {
        log_putc(digits[--n]);
    }
}

static void log_stamp(uint32_t now)
{
    log_putc('[');
    log_u32(now, 7);
    log_puts(" ms] ");
}

// Один байт за ітерацію і тільки якщо передавач вільний. Ніякого очікування:
// не встигли цієї ітерації — зайдемо наступної, цикл через це не стоїть.
static void log_pump()
{
    if (log_tail == log_head) {
        return;
    }
    if (__HAL_UART_GET_FLAG(&huart1, UART_FLAG_TXE) == RESET) {
        return;
    }
    huart1.Instance->DR = static_cast<uint8_t>(log_buf[log_tail]);
    log_tail            = (log_tail + 1) & (LOG_BUF - 1);
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

static void log_mode(uint32_t now, Mode mode, uint32_t presses, uint16_t edges)
{
    const ModeDef &d = def_of(mode);

    log_stamp(now);
    log_puts("press #");
    log_u32(presses);
    log_puts(" -> ");
    log_puts(d.name);
    if (d.blink_ms != 0) {
        log_puts(" (blink ");
        log_u32(d.blink_ms);
        log_puts(" ms)");
    } else {
        log_puts(d.lit ? " (LED on)" : " (LED off)");
    }
    log_puts(", edges filtered: ");
    log_u32(edges);
    log_puts("\r\n");

    if (log_dropped != 0) {
        log_stamp(now);
        log_puts("log overflow, bytes dropped: ");
        log_u32(log_dropped);
        log_puts("\r\n");
        log_dropped = 0;
    }
}

int main()
{
    HAL_Init();
    SystemClock_Config();
    gpio_init();
    uart_init();

    Mode          mode        = Mode::Off;
    bool          led_on      = def_of(mode).lit;
    uint32_t      last_toggle = 0;
    GPIO_PinState last_level  = GPIO_PIN_SET;  // кнопка не натиснута
    uint32_t      last_edge   = 0;
    bool          pressed     = false;

    uint32_t presses = 0;
    uint16_t edges   = 0;  // фронтів від останнього зарахованого натискання

    led_write(led_on);

    log_puts("\r\n=== dz2.8 button_modes_uart ===\r\n");
    log_puts("clock ");
    log_u32(SystemCoreClock);
    log_puts(" Hz, debounce ");
    log_u32(Config::DEBOUNCE_MS);
    log_puts(" ms, slow ");
    log_u32(Config::SLOW_MS);
    log_puts(" ms, fast ");
    log_u32(Config::FAST_MS);
    log_puts(" ms\r\nstart mode ");
    log_puts(def_of(mode).name);
    log_puts(", press KEY on PA0\r\n\r\n");

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

            // Кожен фронт у лог: саме тут видно брязкіт, який гасить витримка.
            edges++;
            log_stamp(now);
            log_puts("edge ");
            log_putc(level == GPIO_PIN_RESET ? '0' : '1');
            log_puts("\r\n");
        } else if (now - last_edge >= Config::DEBOUNCE_MS) {
            const bool stable_pressed = (level == Config::BUTTON_PRESSED);
            if (stable_pressed != pressed) {
                pressed = stable_pressed;
                if (pressed) {  // режим перемикає натискання, не відпускання
                    mode        = next_mode(mode);
                    led_on      = def_of(mode).lit;
                    last_toggle = now;
                    led_write(led_on);

                    presses++;
                    log_mode(now, mode, presses, edges);
                    edges = 0;
                } else {
                    log_stamp(now);
                    log_puts("release\r\n");
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

        // ── лог ──
        log_pump();
    }
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
