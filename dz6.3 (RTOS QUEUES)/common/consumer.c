#include "consumer.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void say(const consumer_t *c, const char *fmt, ...)
{
  char line[96];
  va_list args;
  va_start(args, fmt);
  vsnprintf(line, sizeof line, fmt, args);
  va_end(args);
  c->log(line, c->log_ctx);
}

void consumer_init(consumer_t *c, uint32_t now_ms, log_fn_t log, void *log_ctx)
{
  memset(c, 0, sizeof *c);
  for (int i = 0; i < SOURCE_COUNT; ++i) {
    c->src[i].last_rx_ms = now_ms;
  }
  c->last_stats_ms = now_ms;
  c->log           = log;
  c->log_ctx       = log_ctx;
}

void consumer_note_depth(consumer_t *c, uint32_t depth)
{
  if (depth > QUEUE_LENGTH) {
    depth = QUEUE_LENGTH;
  }
  if (depth > c->peak) {
    c->peak = depth;
  }
}

static void print_value(const consumer_t *c, const sensor_msg_t *msg, uint32_t now_ms)
{
  const char *name = SOURCES[msg->source].name;
  uint32_t    wait = now_ms - msg->time_ms;

  switch (msg->source) {
    case SOURCE_TEMP: {
      int32_t v = msg->value;
      int32_t a = v < 0 ? -v : v;
      say(c, "%-6s #%-4" PRIu32 " %s%" PRId32 ".%02" PRId32 " C   core %u, wait %" PRIu32 " ms",
          name, msg->seq, v < 0 ? "-" : "", a / 100, a % 100, msg->core, wait);
      break;
    }
    case SOURCE_POT:
      say(c, "%-6s #%-4" PRIu32 " %" PRId32 " mV   core %u, wait %" PRIu32 " ms", name, msg->seq,
          msg->value, msg->core, wait);
      break;
    case SOURCE_BUTTON:
      say(c, "%-6s #%-4" PRIu32 " pressed   core %u, wait %" PRIu32 " ms", name, msg->seq,
          msg->core, wait);
      break;
  }
}

void consumer_handle(consumer_t *c, const sensor_msg_t *msg, uint32_t now_ms)
{
  // Повідомлення прийшло ззовні задачі: перевірити межі до звертання до масиву.
  if (msg->source >= SOURCE_COUNT) {
    say(c, "unknown source %u, message dropped", msg->source);
    return;
  }
  source_stats_t *st   = &c->src[msg->source];
  const char     *name = SOURCES[msg->source].name;

  // Черга - FIFO: від одного джерела номери йдуть підряд. Розрив = загублені повідомлення.
  if (msg->seq > st->next_seq) {
    uint32_t gap = msg->seq - st->next_seq;
    st->lost += gap;
    say(c, "%-6s lost %" PRIu32 " before #%" PRIu32, name, gap, msg->seq);
  }
  st->next_seq   = msg->seq + 1;
  st->received++;
  st->last_rx_ms = now_ms;
  if (st->silent) {
    st->silent = false;
    say(c, "%-6s back online", name);
  }

  print_value(c, msg, now_ms);
}

void consumer_tick(consumer_t *c, uint32_t now_ms, uint32_t isr_dropped)
{
  // Тиша перевіряється для кожного датчика окремо. Якщо замовк один датчик, а інший працює,
  // загальний таймаут xQueueReceive не спрацює: повідомлення в черзі є.
  for (int i = 0; i < SOURCE_COUNT; ++i) {
    source_stats_t *st = &c->src[i];
    if (SOURCES[i].period_ms == 0 || st->silent) {
      continue;
    }
    uint32_t window_ms = SILENCE_PERIODS * SOURCES[i].period_ms;
    if (now_ms - st->last_rx_ms >= window_ms) {
      st->silent = true;
      say(c, "%-6s no data for %" PRIu32 " ms", SOURCES[i].name, window_ms);
    }
  }

  if (now_ms - c->last_stats_ms >= STATS_PERIOD_MS) {
    c->last_stats_ms += STATS_PERIOD_MS;
    for (int i = 0; i < SOURCE_COUNT; ++i) {
      say(c, "stats  %-6s received %" PRIu32 ", lost %" PRIu32, SOURCES[i].name,
          c->src[i].received, c->src[i].lost);
    }
    say(c, "stats  queue  peak %" PRIu32 " of %d, ISR dropped %" PRIu32, c->peak, QUEUE_LENGTH,
        isr_dropped);
  }
}
