#include "rate.h"

void rate_init(rate_t *r, uint32_t start)
{
  for (uint32_t i = 0; i < RATE_WINDOW_S; i++) {
    r->past[i] = start;
  }
  r->head    = 0;
  r->seconds = 0;
}

uint32_t rate_update(rate_t *r, uint32_t count)
{
  if (r->seconds < RATE_WINDOW_S) {
    r->seconds++;
  }
  // Різниця беззнакових чисел правильна і після переповнення лічильника.
  uint32_t delta = count - r->past[r->head];  // деталі за останні r->seconds секунд
  r->past[r->head] = count;
  r->head = (r->head + 1) % RATE_WINDOW_S;
  return delta * 600 / r->seconds;  // деталі/хв × 10
}
