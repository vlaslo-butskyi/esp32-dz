#include "button_fsm.h"

void button_init(button_fsm_t *b, uint32_t now_ms)
{
  b->state      = BUTTON_RELEASED;
  b->stable     = false;
  b->candidate  = false;
  b->changed_ms = now_ms;
  b->pressed_ms = now_ms;
  b->overflow   = false;
}

event_t button_step(button_fsm_t *b, bool pressed, uint32_t now_ms, uint32_t *hold_ms)
{
  *hold_ms = 0;

  // Debounce: новий рівень приймається, коли він тримається BUTTON_DEBOUNCE_MS.
  if (pressed != b->candidate) {
    b->candidate  = pressed;
    b->changed_ms = now_ms;
  }
  bool edge = b->candidate != b->stable && now_ms - b->changed_ms >= BUTTON_DEBOUNCE_MS;
  if (edge) {
    b->stable = b->candidate;
  }

  switch (b->state) {
    case BUTTON_RELEASED:
      if (edge && b->stable) {
        b->pressed_ms = b->changed_ms;  // перший відлік нового рівня, а не кінець debounce
        b->state = BUTTON_PRESSED;
        return EVENT_PRESS;
      }
      break;

    case BUTTON_PRESSED:
      if (edge && !b->stable) {
        *hold_ms = b->changed_ms - b->pressed_ms;
        b->state = BUTTON_RELEASED;
        return EVENT_SHORT;
      }
      // candidate == false: кнопку вже відпустили до 1000 мс, debounce ще чекає.
      if (b->candidate && now_ms - b->pressed_ms >= BUTTON_LONG_MS) {
        *hold_ms = now_ms - b->pressed_ms;
        b->state = BUTTON_HELD;
        return EVENT_LONG;
      }
      break;

    case BUTTON_HELD:
      if (edge && !b->stable) {
        b->state = BUTTON_RELEASED;
      }
      break;
  }
  return EVENT_NONE;
}

void button_poll(button_fsm_t *b, bool pressed, uint32_t now_ms, event_sink_t send, void *ctx)
{
  // Автомат кнопки крокує завжди: інакше він пропустить фронт, поки черга повна.
  event_msg_t msg = {EVENT_NONE, 0};
  msg.event = button_step(b, pressed, now_ms, &msg.hold_ms);

  if (b->overflow) {
    event_msg_t overflow = {EVENT_OVERFLOW, 0};
    if (!send(&overflow, ctx)) {
      return;  // черга ще повна: msg теж губиться
    }
    b->overflow = false;
  }

  if (msg.event != EVENT_NONE && !send(&msg, ctx)) {
    b->overflow = true;
  }
}
