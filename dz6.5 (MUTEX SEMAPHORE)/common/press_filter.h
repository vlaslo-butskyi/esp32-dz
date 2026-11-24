// Фільтр натискань для ISR кнопки (ДЗ 6.5), той самий, що в ДЗ 6.3. Датчик деталі - кнопка.
// Без GPIO і без FreeRTOS: рівень піна і час передає ISR, тому код перевіряють тести на ПК.
//
// Переривання спрацьовує на обидва фронти. Брязкіт іде пачкою фронтів без тиші між ними.
// Натискання - перший фронт після тиші PRESS_QUIET_MS, якщо до тиші кнопка була відпущена.
// Рівень у момент ISR для цього не годиться: брязкіт міг уже змінити його знову. Стабільний
// рівень до тиші - це рівень, який ISR прочитав на останньому фронті попередньої пачки.
#ifndef DZ65_PRESS_FILTER_H
#define DZ65_PRESS_FILTER_H

#include <stdbool.h>
#include <stdint.h>

#define PRESS_QUIET_MS UINT32_C(30)  // тиша на піні перед натисканням

typedef struct {
  uint32_t last_edge_ms;  // час останнього фронту
  bool     released;      // рівень на останньому фронті: true = кнопка відпущена
} press_filter_t;

// released - рівень піна під час ініціалізації.
void press_filter_init(press_filter_t *f, bool released);

// Викликати на кожному фронті. released - рівень піна зараз. Повертає true для натискання.
bool press_filter_edge(press_filter_t *f, bool released, uint32_t now_ms);

#endif
