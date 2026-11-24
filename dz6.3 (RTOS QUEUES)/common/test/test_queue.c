// Тести спільного коду ДЗ 6.3 на ПК: ./run.sh
// Кожен тест викликає функції напряму і сам задає час у мілісекундах.

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "consumer.h"
#include "press_filter.h"

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
// Лог consumer: рядки збираються в масив, тест шукає в ньому потрібний.
// ---------------------------------------------------------------------------
static char lines[16][96];
static int  line_count;

static void capture(const char *line, void *ctx)
{
  (void)ctx;
  if (line_count < 16) {
    snprintf(lines[line_count++], sizeof lines[0], "%s", line);
  }
}

static bool logged(const char *text)
{
  for (int i = 0; i < line_count; ++i) {
    if (strstr(lines[i], text) != NULL) {
      return true;
    }
  }
  return false;
}

static void start(consumer_t *c)
{
  line_count = 0;
  consumer_init(c, 0, capture, NULL);
}

// ---------------------------------------------------------------------------
// Фільтр натискань
// ---------------------------------------------------------------------------
static void test_clean_press(void)
{
  press_filter_t f;
  press_filter_init(&f, true);
  CHECK(press_filter_edge(&f, false, 1000) == true);   // натиснули
  CHECK(press_filter_edge(&f, true, 1200) == false);   // відпустили
  CHECK(press_filter_edge(&f, false, 1500) == true);   // натиснули вдруге
}

static void test_press_bounce(void)
{
  // Брязкіт при натисканні: 4 фронти за 3 мс. Натискання - тільки перший фронт.
  press_filter_t f;
  press_filter_init(&f, true);
  CHECK(press_filter_edge(&f, false, 1000) == true);
  CHECK(press_filter_edge(&f, true, 1001) == false);
  CHECK(press_filter_edge(&f, false, 1002) == false);
  CHECK(press_filter_edge(&f, true, 1003) == false);
  CHECK(press_filter_edge(&f, false, 1003) == false);
}

static void test_release_bounce(void)
{
  // Помилка, яку знайшов Wokwi: брязкіт при відпусканні знову опускає пін у 0, і ISR
  // читає «натиснута». Фільтр дивиться на рівень до тиші, тому відпускання - не натискання.
  press_filter_t f;
  press_filter_init(&f, true);
  CHECK(press_filter_edge(&f, false, 1000) == true);   // натиснули
  CHECK(press_filter_edge(&f, false, 1300) == false);  // відпускання, ISR прочитав 0
  CHECK(press_filter_edge(&f, true, 1301) == false);
  CHECK(press_filter_edge(&f, false, 1700) == true);   // наступне справжнє натискання
}

// ---------------------------------------------------------------------------
// Consumer
// ---------------------------------------------------------------------------
static void test_values(void)
{
  consumer_t c;
  start(&c);
  sensor_msg_t temp = {.source = SOURCE_TEMP, .seq = 0, .time_ms = 1000, .value = -105};
  sensor_msg_t pot  = {.source = SOURCE_POT, .core = 1, .seq = 0, .time_ms = 250, .value = 1673};
  sensor_msg_t btn  = {.source = SOURCE_BUTTON, .seq = 0, .time_ms = 1200};
  consumer_handle(&c, &temp, 1003);
  consumer_handle(&c, &pot, 1003);
  consumer_handle(&c, &btn, 1203);

  CHECK(logged("temp   #0    -1.05 C   core 0, wait 3 ms"));
  CHECK(logged("pot    #0    1673 mV   core 1, wait 753 ms"));
  CHECK(logged("button #0    pressed   core 0, wait 3 ms"));
  CHECK(c.src[SOURCE_POT].received == 1);
}

static void test_lost_messages(void)
{
  // Від pot прийшли #0 і #3: #1 і #2 загубились.
  consumer_t c;
  start(&c);
  sensor_msg_t m0 = {.source = SOURCE_POT, .seq = 0};
  sensor_msg_t m3 = {.source = SOURCE_POT, .seq = 3};
  consumer_handle(&c, &m0, 250);
  consumer_handle(&c, &m3, 1000);

  CHECK(logged("pot    lost 2 before #3"));
  CHECK(c.src[SOURCE_POT].lost == 2);
  CHECK(c.src[SOURCE_POT].received == 2);
}

static void test_unknown_source(void)
{
  consumer_t c;
  start(&c);
  sensor_msg_t bad = {.source = 7};
  consumer_handle(&c, &bad, 100);
  CHECK(logged("unknown source 7, message dropped"));
}

static void test_silence(void)
{
  // pot має період 250 мс: тиша 750 мс = датчик зник. temp: 3 x 1000 мс.
  consumer_t c;
  start(&c);
  consumer_tick(&c, 700, 0);
  CHECK(line_count == 0);
  consumer_tick(&c, 750, 0);
  CHECK(logged("pot    no data for 750 ms"));
  CHECK(!logged("temp"));

  consumer_tick(&c, 800, 0);
  CHECK(line_count == 1);  // «no data» виводиться один раз

  sensor_msg_t pot = {.source = SOURCE_POT, .seq = 0, .time_ms = 1000};
  consumer_handle(&c, &pot, 1000);
  CHECK(logged("pot    back online"));

  // Датчик повернувся, тому може замовкнути знову: ще 750 мс тиші - знову «no data».
  line_count = 0;
  consumer_tick(&c, 1750, 0);
  CHECK(logged("pot    no data for 750 ms"));

  consumer_tick(&c, 3000, 0);
  CHECK(logged("temp   no data for 3000 ms"));
}

static void test_stats(void)
{
  consumer_t c;
  start(&c);
  consumer_note_depth(&c, 3);
  consumer_note_depth(&c, 9);  // продюсер на іншому ядрі встиг дописати: більше 8 не буває
  sensor_msg_t pot = {.source = SOURCE_POT, .seq = 0};
  consumer_handle(&c, &pot, 9990);
  line_count = 0;

  consumer_tick(&c, 9999, 0);
  CHECK(!logged("stats"));
  consumer_tick(&c, 10000, 2);
  CHECK(logged("stats  pot    received 1, lost 0"));
  CHECK(logged("stats  queue  peak 8 of 8, ISR dropped 2"));
}

int main(void)
{
  struct {
    const char *name;
    void (*fn)(void);
  } tests[] = {
    {"button: clean press", test_clean_press},
    {"button: bounce on press", test_press_bounce},
    {"button: bounce on release", test_release_bounce},
    {"consumer: values", test_values},
    {"consumer: lost messages", test_lost_messages},
    {"consumer: unknown source", test_unknown_source},
    {"consumer: silence", test_silence},
    {"consumer: stats", test_stats},
  };

  for (size_t i = 0; i < sizeof tests / sizeof tests[0]; ++i) {
    int before = failures;
    tests[i].fn();
    printf("%-28s %s\n", tests[i].name, failures == before ? "ok" : "FAILED");
  }
  printf("%d checks, %d failed\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
