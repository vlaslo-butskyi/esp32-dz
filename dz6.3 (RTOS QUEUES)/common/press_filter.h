// Фільтр натискань для ISR кнопки (ДЗ 6.3). Без GPIO і без FreeRTOS: рівень піна і час
// передає ISR, тому той самий код працює на ESP32-S3, STM32F411 і в тестах на ПК.
//
// Переривання спрацьовує на обидва фронти. Брязкіт іде пачкою фронтів без тиші між ними.
// Натискання - перший фронт після тиші BUTTON_QUIET_MS, якщо до тиші кнопка була відпущена.
// Рівень у момент ISR для цього не годиться: брязкіт міг уже змінити його знову. Стабільний
// рівень до тиші - це рівень, який ISR прочитав на останньому фронті попередньої пачки.
#ifndef DZ63_PRESS_FILTER_H
#define DZ63_PRESS_FILTER_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  uint32_t last_edge_ms;  // час останнього фронту
  bool     released;      // рівень на останньому фронті: true = кнопка відпущена
} press_filter_t;

// released - рівень піна під час ініціалізації.
void press_filter_init(press_filter_t *f, bool released);

// Викликати на кожному фронті. released - рівень піна зараз. Повертає true для натискання.
bool press_filter_edge(press_filter_t *f, bool released, uint32_t now_ms);

#endif
