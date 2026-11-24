// Тести спільного коду ДЗ 6.5 на ПК: ./run.sh
// Кожен тест викликає функції напряму і сам задає час і значення лічильника.

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "press_filter.h"
#include "rate.h"
#include "report.h"

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

// ---------------------------------------------------------------------------
// Фільтр брязкоту датчика деталей
// ---------------------------------------------------------------------------
static void test_part_clean(void)
{
  press_filter_t f;
  press_filter_init(&f, true);
  CHECK(press_filter_edge(&f, false, 1000) == true);   // деталь
  CHECK(press_filter_edge(&f, true, 1200) == false);   // кнопку відпустили
  CHECK(press_filter_edge(&f, false, 1500) == true);   // друга деталь
}

static void test_part_bounce(void)
{
  // 5 фронтів брязкоту за 3 мс при натисканні і 2 при відпусканні: одна деталь.
  press_filter_t f;
  press_filter_init(&f, true);
  CHECK(press_filter_edge(&f, false, 1000) == true);
  CHECK(press_filter_edge(&f, true, 1001) == false);
  CHECK(press_filter_edge(&f, false, 1002) == false);
  CHECK(press_filter_edge(&f, true, 1003) == false);
  CHECK(press_filter_edge(&f, false, 1003) == false);
  CHECK(press_filter_edge(&f, false, 1200) == false);  // відпускання, але брязкіт: ISR прочитав 0
  CHECK(press_filter_edge(&f, true, 1201) == false);
  CHECK(press_filter_edge(&f, false, 1500) == true);   // наступна деталь
}

// ---------------------------------------------------------------------------
// Швидкість конвеєра, деталі/хв × 10
// ---------------------------------------------------------------------------
static void test_rate_first_seconds(void)
{
  // Перші секунди вікно коротше: 2 деталі за 1 с = 120 деталей/хв.
  rate_t r;
  rate_init(&r, 0);
  CHECK(rate_update(&r, 2) == 1200);  // 2 деталі за 1 с
  CHECK(rate_update(&r, 3) == 900);   // 3 деталі за 2 с = 90,0/хв
}

static void test_rate_window(void)
{
  // Одна деталь щосекунди: 60 деталей/хв і в короткому, і в повному вікні 10 с.
  rate_t r;
  rate_init(&r, 0);
  uint32_t x10 = 0;
  for (uint32_t s = 1; s <= 15; s++) {
    x10 = rate_update(&r, s);
    CHECK(x10 == 600);
  }

  // Конвеєр став: через 10 с без деталей швидкість 0, через 5 с - половина.
  for (uint32_t s = 1; s <= 5; s++) {
    x10 = rate_update(&r, 15);
  }
  CHECK(x10 == 300);
  for (uint32_t s = 1; s <= 5; s++) {
    x10 = rate_update(&r, 15);
  }
  CHECK(x10 == 0);
}

static void test_rate_wraparound(void)
{
  // Лічильник перейшов через 0xFFFFFFFF: різниця все одно 3 деталі за 1 с.
  rate_t r;
  rate_init(&r, UINT32_MAX - 1);
  CHECK(rate_update(&r, 1) == 1800);
}

// ---------------------------------------------------------------------------
// Текст звіту
// ---------------------------------------------------------------------------
static void test_report_text(void)
{
  char report[REPORT_SIZE];
  char tail[TAIL_SIZE];

  report_begin(report, sizeof report, 20, 1145);
  CHECK(strcmp(report, "parts 20, rate 114.5/min") == 0);

  report_pot(tail, sizeof tail, 3134);
  CHECK(strcmp(tail, "; pot 3134 mV") == 0);

  CHECK(report_append(report, sizeof report, tail) == true);
  CHECK(strcmp(report, "parts 20, rate 114.5/min; pot 3134 mV") == 0);
}

static void test_report_full(void)
{
  // Хвіст не вміщується: звіт лишається без змін, а не обрізаним.
  char report[16] = "parts 1";
  CHECK(report_append(report, sizeof report, "; pot 3134 mV") == false);
  CHECK(strcmp(report, "parts 1") == 0);

  // 16 символів у 16 байт не вміщуються: для '\0' місця немає.
  CHECK(report_append(report, sizeof report, "123456789") == false);

  // Рівно на межі: 15 символів і '\0' вміщуються в 16 байт.
  CHECK(report_append(report, sizeof report, "12345678") == true);
  CHECK(strlen(report) == 15);
}

int main(void)
{
  struct {
    const char *name;
    void (*fn)(void);
  } tests[] = {
    {"part: clean press", test_part_clean},
    {"part: bounce", test_part_bounce},
    {"rate: first seconds", test_rate_first_seconds},
    {"rate: 10 s window", test_rate_window},
    {"rate: counter wraparound", test_rate_wraparound},
    {"report: text", test_report_text},
    {"report: buffer full", test_report_full},
  };

  for (size_t i = 0; i < sizeof tests / sizeof tests[0]; ++i) {
    int before = failures;
    tests[i].fn();
    printf("%-26s %s\n", tests[i].name, failures == before ? "ok" : "FAILED");
  }
  printf("%d checks, %d failed\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
