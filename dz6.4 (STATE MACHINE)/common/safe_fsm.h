// Автомат сейфа (Consumer) для ДЗ 6.4. Пароль: Коротке - Коротке - Довге.
// Без FreeRTOS і без GPIO: подію і час передає виклик.
//
//   LOCKED  --SHORT--> SHORT_1
//   SHORT_1 --SHORT--> SHORT_2
//   SHORT_2 --LONG---> OPEN
//   OPEN    --SHORT або TIMEOUT 5 с--> LOCKED
//
//   SHORT_1, SHORT_2: неправильна подія або TIMEOUT 3 с --> LOCKED
//   LOCKED: LONG --> LOCKED (неправильна подія, лічити нічого)
//   будь-який стан: OVERFLOW --> LOCKED
//   PRESS ніде не змінює стан. Він тільки перезапускає таймаут.
#ifndef DZ64_SAFE_FSM_H
#define DZ64_SAFE_FSM_H

#include <stddef.h>
#include <stdint.h>

#include "button_fsm.h"
#include "events.h"

enum {
  SAFE_INPUT_TIMEOUT_MS = 3000,  // «затупив»: тиша між подіями
  SAFE_OPEN_MS          = 5000,  // сейф сам закривається
};

// Дві різні анонімні enum: gcc не дає порівнювати їх без приведення.
_Static_assert((int)SAFE_INPUT_TIMEOUT_MS > (int)BUTTON_LONG_MS,
               "LONG must arrive before the input timeout, counted from PRESS");

#define SAFE_WAIT_FOREVER UINT32_MAX

typedef enum {
  SAFE_LOCKED,
  SAFE_SHORT_1,
  SAFE_SHORT_2,
  SAFE_OPEN,
} safe_state_t;

typedef enum {
  OUTCOME_STAY,      // подія нічого не змінює
  OUTCOME_STEP,      // правильний крок пароля
  OUTCOME_UNLOCK,    // пароль введено
  OUTCOME_WRONG,     // неправильна подія, скидання
  OUTCOME_EXPIRED,   // таймаут, скидання
  OUTCOME_OVERFLOW,  // черга загубила подію, скидання
  OUTCOME_CLOSE,     // сейф закрито
} safe_outcome_t;

typedef struct {
  safe_state_t   next;
  safe_outcome_t outcome;
} safe_transition_t;

// Таблиця переходів: чиста функція пари «стан, подія».
safe_transition_t safe_step(safe_state_t state, event_t event);

// Скільки стан чекає наступної події. 0 - без таймауту.
uint32_t safe_timeout_ms(safe_state_t state);

const char *safe_state_name(safe_state_t state);

// Автомат разом з таймаутом: стан і момент останньої події з черги.
typedef struct {
  safe_state_t state;
  uint32_t     last_event_ms;
} safe_t;

void safe_init(safe_t *s, uint32_t now_ms);

// Скільки ще чекати подію до таймауту. SAFE_WAIT_FOREVER - у стані без таймауту.
uint32_t safe_wait_ms(const safe_t *s, uint32_t now_ms);

// Обробити подію з черги або EVENT_TIMEOUT. Кожна подія з черги, також PRESS,
// перезапускає таймаут. Повертає перехід; новий стан уже записано в s->state.
safe_transition_t safe_handle(safe_t *s, const event_msg_t *msg, uint32_t now_ms);

// Рядок для логу без часу і без рівня. Повертає рівень: 'I' або 'W'.
char safe_describe(char *buf, size_t size, const event_msg_t *msg, safe_state_t from,
                   safe_transition_t t);

#endif
