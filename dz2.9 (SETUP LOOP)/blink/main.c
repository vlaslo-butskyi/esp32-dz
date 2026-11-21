// ДЗ 2.9 — платформенний рівень: усе, що стосується заліза й HAL.
//
// Після винесення логіки в app.c тут не лишилось жодного рядка про
// світлодіод — тільки старт мікроконтролера й виклик прикладного коду. Саме
// такий main.c генерує CubeMX: при повторній генерації він переписується, і
// все, що лежало б у ньому поза блоками USER CODE, зникло б. Бізнес-логіка в
// окремому файлі цієї генерації не бачить взагалі.
//
// Структура один в один повторює те, що робить Ардуіно-фреймворк: його
// main.cpp теж складається з ініціалізації, одного setup() і нескінченного
// циклу з loop().

#include "stm32f4xx_hal.h"

#include "app.h"

static void SystemClock_Config(void);
static void Error_Handler(void);

// PlatformIO з framework = stm32cube не кладе згенерований stm32f4xx_it.c,
// тому обробник системного таймера треба оголосити самому. Без нього
// спрацьовує слабка заглушка зі startup_*.s — нескінченний цикл, —
// HAL_GetTick() назавжди лишається нулем, а HAL_Delay() не повертається.
//
// У ДЗ 2.8 тут стояло extern "C": той main був .cpp, і без обгортки ім'я
// обробника не збіглося б із таблицею векторів. Цей файл — C, тож обгортка
// не потрібна; натомість вона є в app.h, щоб заголовок можна було включити
// і з C++ теж.
void SysTick_Handler(void)
{
    HAL_IncTick();
}

int main(void)
{
    HAL_Init();
    SystemClock_Config();

    setup();

    while (1) {
        loop();
    }
}

static void SystemClock_Config(void)
{
    // Тактування від внутрішнього HSI 16 МГц, без PLL — як у ДЗ 2.8. Для
    // блимання більше не потрібно, зате ця конфігурація однакова для F401 і
    // F411: межі PLL у них різні (84 і 100 МГц), спільного набору дільників
    // немає.
    RCC_OscInitTypeDef osc = {0};
    osc.OscillatorType      = RCC_OSCILLATORTYPE_HSI;
    osc.HSIState            = RCC_HSI_ON;
    osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    osc.PLL.PLLState        = RCC_PLL_NONE;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
        Error_Handler();
    }

    RCC_ClkInitTypeDef clk = {0};
    clk.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_PCLK1 |
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

static void Error_Handler(void)
{
    __disable_irq();
    while (1) {
    }
}
