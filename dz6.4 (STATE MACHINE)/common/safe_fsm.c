#include "safe_fsm.h"

#include <inttypes.h>
#include <stdio.h>

safe_transition_t safe_step(safe_state_t state, event_t event)
{
  // Втрата події ламає послідовність у будь-якому стані: безпечніше закрити сейф.
  if (event == EVENT_OVERFLOW) {
    return (safe_transition_t){SAFE_LOCKED, OUTCOME_OVERFLOW};
  }

  switch (state) {
    case SAFE_LOCKED:
      if (event == EVENT_SHORT) return (safe_transition_t){SAFE_SHORT_1, OUTCOME_STEP};
      if (event == EVENT_LONG)  return (safe_transition_t){SAFE_LOCKED,  OUTCOME_WRONG};
      return (safe_transition_t){SAFE_LOCKED, OUTCOME_STAY};

    case SAFE_SHORT_1:
      if (event == EVENT_SHORT)   return (safe_transition_t){SAFE_SHORT_2, OUTCOME_STEP};
      if (event == EVENT_LONG)    return (safe_transition_t){SAFE_LOCKED,  OUTCOME_WRONG};
      if (event == EVENT_TIMEOUT) return (safe_transition_t){SAFE_LOCKED,  OUTCOME_EXPIRED};
      return (safe_transition_t){SAFE_SHORT_1, OUTCOME_STAY};

    case SAFE_SHORT_2:
      if (event == EVENT_LONG)    return (safe_transition_t){SAFE_OPEN,   OUTCOME_UNLOCK};
      if (event == EVENT_SHORT)   return (safe_transition_t){SAFE_LOCKED, OUTCOME_WRONG};
      if (event == EVENT_TIMEOUT) return (safe_transition_t){SAFE_LOCKED, OUTCOME_EXPIRED};
      return (safe_transition_t){SAFE_SHORT_2, OUTCOME_STAY};

    case SAFE_OPEN:
      if (event == EVENT_SHORT || event == EVENT_TIMEOUT) {
        return (safe_transition_t){SAFE_LOCKED, OUTCOME_CLOSE};
      }
      return (safe_transition_t){SAFE_OPEN, OUTCOME_STAY};
  }
  return (safe_transition_t){SAFE_LOCKED, OUTCOME_WRONG};  // недосяжно: стан поза enum
}

uint32_t safe_timeout_ms(safe_state_t state)
{
  switch (state) {
    case SAFE_SHORT_1:
    case SAFE_SHORT_2: return SAFE_INPUT_TIMEOUT_MS;
    case SAFE_OPEN:    return SAFE_OPEN_MS;
    case SAFE_LOCKED:  return 0;
  }
  return 0;
}

const char *safe_state_name(safe_state_t state)
{
  switch (state) {
    case SAFE_LOCKED:  return "LOCKED";
    case SAFE_SHORT_1: return "SHORT_1";
    case SAFE_SHORT_2: return "SHORT_2";
    case SAFE_OPEN:    return "OPEN";
  }
  return "?";
}

void safe_init(safe_t *s, uint32_t now_ms)
{
  s->state         = SAFE_LOCKED;
  s->last_event_ms = now_ms;
}

uint32_t safe_wait_ms(const safe_t *s, uint32_t now_ms)
{
  uint32_t limit = safe_timeout_ms(s->state);
  if (limit == 0) {
    return SAFE_WAIT_FOREVER;
  }
  uint32_t passed = now_ms - s->last_event_ms;
  return passed >= limit ? 0 : limit - passed;
}

safe_transition_t safe_handle(safe_t *s, const event_msg_t *msg, uint32_t now_ms)
{
  // Таймаут рахується від останньої події з черги, PRESS теж рахується. Тому автомат
  // не скинеться, поки користувач тримає кнопку і чекає на LONG.
  if (msg->event != EVENT_TIMEOUT) {
    s->last_event_ms = now_ms;
  }
  safe_transition_t t = safe_step(s->state, msg->event);
  s->state = t.next;
  return t;
}

char safe_describe(char *buf, size_t size, const event_msg_t *msg, safe_state_t from,
                   safe_transition_t t)
{
  const char *ev = event_name(msg->event);
  const char *a  = safe_state_name(from);
  const char *b  = safe_state_name(t.next);

  // Тривалість мають SHORT, LONG і TIMEOUT. Іншим подіям - пробіли, щоб стовпці стояли рівно.
  char dur[16] = "       ";
  if (msg->event == EVENT_TIMEOUT) {
    snprintf(dur, sizeof dur, "%4" PRIu32 " ms", safe_timeout_ms(from));
  } else if (msg->event == EVENT_SHORT || msg->event == EVENT_LONG) {
    snprintf(dur, sizeof dur, "%4" PRIu32 " ms", msg->hold_ms);
  }

  switch (t.outcome) {
    case OUTCOME_STAY:
      snprintf(buf, size, "%-8s %s  %s%s", ev, dur, a,
               safe_timeout_ms(from) > 0 ? ", timer restarted" : "");
      return 'I';
    case OUTCOME_STEP:
      snprintf(buf, size, "%-8s %s  %s -> %s", ev, dur, a, b);
      return 'I';
    case OUTCOME_UNLOCK:
      snprintf(buf, size, "%-8s %s  %s -> %s, safe is open", ev, dur, a, b);
      return 'I';
    case OUTCOME_CLOSE:
      snprintf(buf, size, "%-8s %s  %s -> %s, safe is closed", ev, dur, a, b);
      return 'I';
    case OUTCOME_WRONG:
      snprintf(buf, size, "%-8s %s  %s -> %s, wrong input", ev, dur, a, b);
      return 'W';
    case OUTCOME_EXPIRED:
      snprintf(buf, size, "%-8s %s  %s -> %s, input timeout", ev, dur, a, b);
      return 'W';
    case OUTCOME_OVERFLOW:
      snprintf(buf, size, "%-8s %s  %s -> %s, event lost, reset", ev, dur, a, b);
      return 'W';
  }
  snprintf(buf, size, "%-8s ?", ev);
  return 'W';
}
