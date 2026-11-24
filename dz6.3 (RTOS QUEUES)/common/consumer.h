// Логіка consumer для ДЗ 6.3: облік повідомлень, втрат і тиші датчиків, рядки логу.
// Без FreeRTOS і без printf: готовий рядок consumer віддає функції log, яку дає прошивка.
// Час передає виклик. Тому той самий код працює на ESP32-S3, STM32F411 і в тестах на ПК.
#ifndef DZ63_CONSUMER_H
#define DZ63_CONSUMER_H

#include <stdbool.h>
#include <stdint.h>

#include "sensor_msg.h"

// Отримує один рядок логу без часу і без переводу рядка.
typedef void (*log_fn_t)(const char *line, void *ctx);

typedef struct {
  uint32_t received;
  uint32_t lost;
  uint32_t next_seq;
  uint32_t last_rx_ms;
  bool     silent;
} source_stats_t;

typedef struct {
  source_stats_t src[SOURCE_COUNT];
  uint32_t       peak;           // найбільша глибина черги
  uint32_t       last_stats_ms;  // коли востаннє виводилась статистика
  log_fn_t       log;
  void          *log_ctx;
} consumer_t;

void consumer_init(consumer_t *c, uint32_t now_ms, log_fn_t log, void *log_ctx);

// Глибина черги разом із щойно взятим повідомленням. Більше QUEUE_LENGTH не буває:
// продюсер на іншому ядрі міг дописати повідомлення між xQueueReceive і підрахунком.
void consumer_note_depth(consumer_t *c, uint32_t depth);

// Обробити повідомлення з черги: перевірити джерело, порахувати розрив у seq, вивести значення.
void consumer_handle(consumer_t *c, const sensor_msg_t *msg, uint32_t now_ms);

// Викликати після кожного xQueueReceive, також після таймауту. Перевіряє тишу кожного
// датчика окремо і раз на STATS_PERIOD_MS виводить статистику.
void consumer_tick(consumer_t *c, uint32_t now_ms, uint32_t isr_dropped);

#endif
