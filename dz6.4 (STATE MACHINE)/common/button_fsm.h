// Автомат кнопки (Producer) для ДЗ 6.4. Без GPIO і без FreeRTOS: рівень піна і час
// передає виклик, тому той самий код працює на ESP32-S3, STM32F411 і в тестах на ПК.
//
//   RELEASED --натиснута--> PRESSED      [PRESS]
//   PRESSED  --відпущена--> RELEASED     [SHORT]
//   PRESSED  --1000 мс----> HELD         [LONG]
//   HELD     --відпущена--> RELEASED
//
// «Натиснута» і «відпущена» - рівень після debounce: брязкіт не дає зайвих подій ні при
// натисканні, ні при відпусканні. Увесь час - різниця мілісекунд і порівняння через >=,
// тому інший період опитування не ламає інтервали.
#ifndef DZ64_BUTTON_FSM_H
#define DZ64_BUTTON_FSM_H

#include <stdbool.h>
#include <stdint.h>

#include "events.h"

enum {
  BUTTON_POLL_MS     = 10,    // період опитування на платі
  BUTTON_DEBOUNCE_MS = 30,    // стільки рівень має не змінюватися
  BUTTON_LONG_MS     = 1000,  // утримання від цього часу = LONG
};

_Static_assert(BUTTON_DEBOUNCE_MS >= 2 * BUTTON_POLL_MS, "debounce needs at least 2 polls");
_Static_assert(BUTTON_LONG_MS > BUTTON_DEBOUNCE_MS, "a long press must outlast debounce");

typedef enum {
  BUTTON_RELEASED,
  BUTTON_PRESSED,
  BUTTON_HELD,
} button_state_t;

typedef struct {
  button_state_t state;
  bool     stable;      // рівень після debounce: true = натиснута
  bool     candidate;   // останній прочитаний рівень
  uint32_t changed_ms;  // коли candidate змінився востаннє
  uint32_t pressed_ms;  // коли почалося натискання
  bool     overflow;    // черга відкинула подію, сейф ще не отримав EVENT_OVERFLOW
} button_fsm_t;

void button_init(button_fsm_t *b, uint32_t now_ms);

// Один крок автомата: рівень піна в момент now_ms -> подія або EVENT_NONE.
// hold_ms отримує тривалість натискання для SHORT і LONG.
event_t button_step(button_fsm_t *b, bool pressed, uint32_t now_ms, uint32_t *hold_ms);

// Куди button_poll кладе подію. Повертає false, якщо черга повна.
typedef bool (*event_sink_t)(const event_msg_t *msg, void *ctx);

// button_step плюс доставка в чергу. Якщо черга відкинула подію, наступні виклики
// спершу намагаються доставити EVENT_OVERFLOW. Поки EVENT_OVERFLOW не доставлено,
// нові події теж губляться: вони вже належать до зламаної послідовності.
void button_poll(button_fsm_t *b, bool pressed, uint32_t now_ms, event_sink_t send, void *ctx);

#endif
