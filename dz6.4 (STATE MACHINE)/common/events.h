// Події між автоматом кнопки (Producer) і автоматом сейфа (Consumer).
// Спільний код ДЗ 6.4: без FreeRTOS, без GPIO, збирається для ESP32-S3, STM32F411 і ПК.
#ifndef DZ64_EVENTS_H
#define DZ64_EVENTS_H

#include <stdint.h>

typedef enum {
  EVENT_NONE,      // нічого не сталося; button_step повертає її майже на кожному опитуванні
  EVENT_PRESS,     // кнопку натиснули; перезапускає таймаут сейфа
  EVENT_SHORT,     // кнопку відпустили раніше за BUTTON_LONG_MS
  EVENT_LONG,      // кнопку тримають BUTTON_LONG_MS
  EVENT_TIMEOUT,   // створює сам сейф, коли подія не прийшла вчасно
  EVENT_OVERFLOW,  // черга відкинула подію: послідовність натискань неповна
} event_t;

// Елемент черги. Черга копіює його цілим.
typedef struct {
  event_t  event;
  uint32_t hold_ms;  // SHORT і LONG: скільки кнопку тримали; інші події: 0
} event_msg_t;

static inline const char *event_name(event_t event)
{
  switch (event) {
    case EVENT_NONE:     return "NONE";
    case EVENT_PRESS:    return "PRESS";
    case EVENT_SHORT:    return "SHORT";
    case EVENT_LONG:     return "LONG";
    case EVENT_TIMEOUT:  return "TIMEOUT";
    case EVENT_OVERFLOW: return "OVERFLOW";
  }
  return "?";
}

#endif
