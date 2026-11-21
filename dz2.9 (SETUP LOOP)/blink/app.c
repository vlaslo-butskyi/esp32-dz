// ДЗ 2.9 — бізнес-логіка: блимання світлодіодом.
//
// Код з уроку був такий:
//
//     HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin);
//     HAL_Delay(BLINK_INTERVAL_MS);
//
// і лежав прямо в main() між /* USER CODE BEGIN 3 */ і /* USER CODE END 3 */.
// Завдання 2.9 — винести його звідти в setup() і loop(), як в Ардуіно. Тіло
// перенесено дослівно: ця домашка про розділення файлів, а не про переписування
// логіки.
//
// Поділ пройшов по тому, що робиться один раз, і тому, що робиться завжди:
// налаштування піна — у setup(), перемикання рівня — у loop().

#include "app.h"

#include "stm32f4xx_hal.h"

// У CubeMX ці імена генеруються з міток пінів у main.h. Збірка тут без
// генератора, тож визначаю їх сам — як у ДЗ 2.8, щоб код з уроку лишився
// дослівно впізнаваним.
#define LED_GPIO_Port GPIOC
#define LED_Pin       GPIO_PIN_13

// Світлодіод на Black Pill сидить катодом на PC13, анодом через резистор на
// 3,3 В — тобто горить від НИЗЬКОГО рівня.
#define LED_ON  GPIO_PIN_RESET
#define LED_OFF GPIO_PIN_SET

#define BLINK_INTERVAL_MS 500u

// HAL_Delay() відмірює час у тиках SysTick, а тик тут — 1 мс. Перший тик після
// виклику вже міг бути на підході, тому HAL_Delay(n) чекає від n до n+1 мс:
// щоб ця похибка була непомітною, період має бути на порядок більшим за тик.
_Static_assert(BLINK_INTERVAL_MS >= 10,
               "period comparable to the 1 ms SysTick makes the blink visibly uneven");

void setup(void)
{
    __HAL_RCC_GPIOC_CLK_ENABLE();

    // Гасимо ДО переведення піна у вихід: інакше між перемиканням режиму піна
    // і першим записом світлодіод коротко спалахнув би.
    HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, LED_OFF);

    GPIO_InitTypeDef led = {0};
    led.Pin   = LED_Pin;
    led.Mode  = GPIO_MODE_OUTPUT_PP;
    led.Pull  = GPIO_NOPULL;
    led.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(LED_GPIO_Port, &led);
}

void loop(void)
{
    HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin);
    HAL_Delay(BLINK_INTERVAL_MS);
}
