#include "report.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

void report_begin(char *out, size_t size, uint32_t parts, uint32_t rate_x10)
{
  snprintf(out, size, "parts %" PRIu32 ", rate %" PRIu32 ".%" PRIu32 "/min", parts,
           rate_x10 / 10, rate_x10 % 10);
}

void report_pot(char *out, size_t size, int32_t mv)
{
  snprintf(out, size, "; pot %" PRId32 " mV", mv);
}

bool report_append(char *report, size_t size, const char *tail)
{
  size_t len = strlen(report);
  size_t add = strlen(tail);
  if (len + add >= size) {
    return false;
  }
  memcpy(report + len, tail, add + 1);  // разом із завершальним '\0'
  return true;
}
