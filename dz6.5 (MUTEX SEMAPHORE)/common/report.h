// Текст системного звіту (ДЗ 6.5). Без FreeRTOS і без mutex: буфер захищає прошивка,
// ці функції тільки форматують рядки. Звіт за одну секунду складають дві задачі:
//   report (Task 1) пише початок:   "parts 7, rate 36.0/min"
//   adc    (Task 2) дописує хвіст:  "; pot 1673 mV"
//   estop  (Task 3) замінює звіт:   "SYSTEM HALTED!"
#ifndef DZ65_REPORT_H
#define DZ65_REPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define REPORT_SIZE   64
#define TAIL_SIZE     24
#define REPORT_HALTED "SYSTEM HALTED!"

// Початок звіту. rate_x10 - деталі/хв × 10, як повертає rate_update().
void report_begin(char *out, size_t size, uint32_t parts, uint32_t rate_x10);

// Хвіст звіту з напругою потенціометра.
void report_pot(char *out, size_t size, int32_t mv);

// Дописати tail у кінець report. Якщо не вміщується, report не змінюється і функція
// повертає false: обрізаний звіт гірший за звіт без хвоста.
bool report_append(char *report, size_t size, const char *tail);

#endif
