#include "press_filter.h"

void press_filter_init(press_filter_t *f, bool released)
{
  f->last_edge_ms = 0;
  f->released     = released;
}

bool press_filter_edge(press_filter_t *f, bool released, uint32_t now_ms)
{
  bool quiet        = now_ms - f->last_edge_ms >= PRESS_QUIET_MS;
  bool was_released = f->released;
  f->last_edge_ms   = now_ms;
  f->released       = released;
  return quiet && was_released;
}
