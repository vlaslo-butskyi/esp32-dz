// Тести спільного коду ДЗ 6.4 на ПК: ./run.sh
// Кожен тест викликає функції автомата напряму і сам задає час у мілісекундах.

#include <stdbool.h>
#include <stdio.h>

#include "button_fsm.h"
#include "safe_fsm.h"

static int checks;
static int failures;

#define CHECK(cond) check((cond), #cond, __LINE__)

static void check(bool ok, const char *text, int line)
{
  checks++;
  if (!ok) {
    failures++;
    printf("  FAIL line %d: %s\n", line, text);
  }
}

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

// ---------------------------------------------------------------------------
// Таблиця переходів сейфа
// ---------------------------------------------------------------------------
static safe_state_t feed(safe_state_t state, const event_t *events, size_t n)
{
  for (size_t i = 0; i < n; ++i) {
    state = safe_step(state, events[i]).next;
  }
  return state;
}

static void test_table(void)
{
  const event_t password[]      = {EVENT_SHORT, EVENT_SHORT, EVENT_LONG};
  const event_t with_press[]    = {EVENT_PRESS, EVENT_SHORT, EVENT_PRESS, EVENT_SHORT,
                                   EVENT_PRESS, EVENT_LONG};
  const event_t long_first[]    = {EVENT_LONG};
  const event_t short_long[]    = {EVENT_SHORT, EVENT_LONG};
  const event_t three_short[]   = {EVENT_SHORT, EVENT_SHORT, EVENT_SHORT, EVENT_LONG};
  const event_t retry[]         = {EVENT_SHORT, EVENT_LONG, EVENT_SHORT, EVENT_SHORT, EVENT_LONG};
  const event_t timeout[]       = {EVENT_SHORT, EVENT_SHORT, EVENT_TIMEOUT, EVENT_LONG};
  const event_t lost_short[]    = {EVENT_SHORT, EVENT_OVERFLOW, EVENT_SHORT, EVENT_LONG};

  CHECK(feed(SAFE_LOCKED, password, COUNT(password)) == SAFE_OPEN);
  CHECK(feed(SAFE_LOCKED, with_press, COUNT(with_press)) == SAFE_OPEN);
  CHECK(feed(SAFE_LOCKED, long_first, COUNT(long_first)) == SAFE_LOCKED);
  CHECK(feed(SAFE_LOCKED, short_long, COUNT(short_long)) == SAFE_LOCKED);
  CHECK(feed(SAFE_LOCKED, three_short, COUNT(three_short)) == SAFE_LOCKED);
  CHECK(feed(SAFE_LOCKED, retry, COUNT(retry)) == SAFE_OPEN);
  CHECK(feed(SAFE_LOCKED, timeout, COUNT(timeout)) == SAFE_LOCKED);
  CHECK(feed(SAFE_LOCKED, lost_short, COUNT(lost_short)) == SAFE_LOCKED);

  // PRESS не змінює жоден стан, OVERFLOW з будь-якого стану веде в LOCKED.
  const safe_state_t all[] = {SAFE_LOCKED, SAFE_SHORT_1, SAFE_SHORT_2, SAFE_OPEN};
  for (size_t i = 0; i < COUNT(all); ++i) {
    CHECK(safe_step(all[i], EVENT_PRESS).next == all[i]);
    CHECK(safe_step(all[i], EVENT_OVERFLOW).next == SAFE_LOCKED);
  }
}

// ---------------------------------------------------------------------------
// Автомат кнопки
// ---------------------------------------------------------------------------

// Опитує кнопку кожні 10 мс від from до to (не включно), рівень увесь час однаковий.
// Повертає першу подію, яка не EVENT_NONE. Якщо подій немає - EVENT_NONE.
static event_t poll(button_fsm_t *b, bool pressed, uint32_t from, uint32_t to, uint32_t *hold)
{
  event_t found = EVENT_NONE;
  for (uint32_t t = from; t < to; t += 10) {
    uint32_t h = 0;
    event_t e = button_step(b, pressed, t, &h);
    if (e != EVENT_NONE && found == EVENT_NONE) {
      found = e;
      *hold = h;
    }
  }
  return found;
}

static void test_button_short(void)
{
  button_fsm_t b;
  uint32_t hold = 0;
  button_init(&b, 0);

  CHECK(poll(&b, true, 0, 30, &hold) == EVENT_NONE);    // debounce ще чекає
  CHECK(poll(&b, true, 30, 40, &hold) == EVENT_PRESS);  // 30 мс без змін
  CHECK(poll(&b, true, 40, 150, &hold) == EVENT_NONE);
  CHECK(poll(&b, false, 150, 200, &hold) == EVENT_SHORT);
  CHECK(hold == 150);
}

static void test_button_long(void)
{
  button_fsm_t b;
  uint32_t hold = 0;
  button_init(&b, 0);

  CHECK(poll(&b, true, 0, 1000, &hold) == EVENT_PRESS);
  CHECK(poll(&b, true, 1000, 1010, &hold) == EVENT_LONG);  // не чекає на відпускання
  CHECK(hold == 1000);
  CHECK(poll(&b, false, 1500, 1600, &hold) == EVENT_NONE);  // після LONG відпускання тихе
}

static void test_button_bounce(void)
{
  // Брязкіт при натисканні: рівень стрибає кожні 2 мс, потім стабільно натиснута.
  button_fsm_t b;
  uint32_t hold = 0;
  button_init(&b, 0);

  CHECK(button_step(&b, true, 0, &hold) == EVENT_NONE);
  CHECK(button_step(&b, false, 2, &hold) == EVENT_NONE);
  CHECK(button_step(&b, true, 4, &hold) == EVENT_NONE);
  CHECK(button_step(&b, false, 6, &hold) == EVENT_NONE);
  CHECK(button_step(&b, true, 8, &hold) == EVENT_NONE);
  CHECK(button_step(&b, true, 30, &hold) == EVENT_NONE);   // від 8 мс минуло 22 мс
  CHECK(button_step(&b, true, 38, &hold) == EVENT_PRESS);  // від 8 мс минуло 30 мс
}

static void test_button_glitch(void)
{
  // Контакт розімкнувся на 20 мс посеред утримання: це не відпускання.
  button_fsm_t b;
  uint32_t hold = 0;
  button_init(&b, 0);

  CHECK(poll(&b, true, 0, 500, &hold) == EVENT_PRESS);
  CHECK(poll(&b, false, 500, 520, &hold) == EVENT_NONE);  // 20 мс < debounce 30 мс
  CHECK(poll(&b, true, 520, 1010, &hold) == EVENT_LONG);
}

static void test_button_release_at_990(void)
{
  // Відпустили на 990 мс. Debounce підтвердить відпускання вже після 1000 мс,
  // але подія має бути SHORT, а не LONG.
  button_fsm_t b;
  uint32_t hold = 0;
  button_init(&b, 0);

  CHECK(poll(&b, true, 0, 990, &hold) == EVENT_PRESS);
  CHECK(poll(&b, false, 990, 1100, &hold) == EVENT_SHORT);
  CHECK(hold == 990);
}

// ---------------------------------------------------------------------------
// Таймаут сейфа
// ---------------------------------------------------------------------------
static const event_msg_t PRESS = {EVENT_PRESS, 0};
static const event_msg_t SHORT = {EVENT_SHORT, 150};
static const event_msg_t LONG  = {EVENT_LONG, 1000};
static const event_msg_t TIMEOUT = {EVENT_TIMEOUT, 0};

static void test_timeout(void)
{
  safe_t s;
  safe_init(&s, 0);
  CHECK(safe_wait_ms(&s, 100000) == SAFE_WAIT_FOREVER);  // у LOCKED таймауту немає

  safe_handle(&s, &SHORT, 1000);
  CHECK(safe_wait_ms(&s, 1000) == 3000);
  CHECK(safe_wait_ms(&s, 3500) == 500);
  CHECK(safe_wait_ms(&s, 4000) == 0);

  safe_transition_t t = safe_handle(&s, &TIMEOUT, 4000);
  CHECK(t.next == SAFE_LOCKED);
  CHECK(t.outcome == OUTCOME_EXPIRED);
}

static void test_press_restarts_timeout(void)
{
  // Пастка з умови: S, S, пауза 2500 мс, утримання. LONG приходить через 3500 мс
  // після останнього SHORT. Таймаут рахується від PRESS, тому сейф відкривається.
  safe_t s;
  safe_init(&s, 0);
  safe_handle(&s, &SHORT, 1000);
  safe_handle(&s, &SHORT, 1400);
  safe_handle(&s, &PRESS, 3900);
  CHECK(safe_wait_ms(&s, 4900) == 2000);  // від PRESS, а не від SHORT

  safe_transition_t t = safe_handle(&s, &LONG, 4900);
  CHECK(t.next == SAFE_OPEN);
}

static void test_open_closes_itself(void)
{
  safe_t s;
  safe_init(&s, 0);
  safe_handle(&s, &SHORT, 1000);
  safe_handle(&s, &SHORT, 1400);
  safe_handle(&s, &LONG, 2800);
  CHECK(s.state == SAFE_OPEN);
  CHECK(safe_wait_ms(&s, 2800) == 5000);
}

// ---------------------------------------------------------------------------
// Переповнення черги
// ---------------------------------------------------------------------------
static int     reject;     // скільки наступних send відкинути
static event_t sent[8];    // що дійшло до черги
static int     sent_count;

static bool fake_send(const event_msg_t *msg, void *ctx)
{
  (void)ctx;
  if (reject > 0) {
    reject--;
    return false;
  }
  sent[sent_count++] = msg->event;
  return true;
}

static void test_overflow(void)
{
  button_fsm_t b;
  button_init(&b, 0);
  sent_count = 0;

  for (uint32_t t = 0; t < 150; t += 10) {
    button_poll(&b, true, t, fake_send, NULL);  // PRESS на 30 мс
  }
  reject = 1;  // черга відкине SHORT
  for (uint32_t t = 150; t < 250; t += 10) {
    button_poll(&b, false, t, fake_send, NULL);  // SHORT на 180 мс, OVERFLOW на 190 мс
  }

  CHECK(sent_count == 2);
  CHECK(sent[0] == EVENT_PRESS);
  CHECK(sent[1] == EVENT_OVERFLOW);
}

int main(void)
{
  struct {
    const char *name;
    void (*fn)(void);
  } tests[] = {
    {"table", test_table},
    {"button: short press", test_button_short},
    {"button: long press", test_button_long},
    {"button: bounce", test_button_bounce},
    {"button: glitch while held", test_button_glitch},
    {"button: release at 990 ms", test_button_release_at_990},
    {"safe: input timeout", test_timeout},
    {"safe: PRESS restarts the timeout", test_press_restarts_timeout},
    {"safe: open closes itself", test_open_closes_itself},
    {"queue overflow", test_overflow},
  };

  for (size_t i = 0; i < COUNT(tests); ++i) {
    int before = failures;
    tests[i].fn();
    printf("%-34s %s\n", tests[i].name, failures == before ? "ok" : "FAILED");
  }
  printf("%d checks, %d failed\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
