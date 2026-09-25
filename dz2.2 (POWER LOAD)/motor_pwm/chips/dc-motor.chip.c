// Двигун постійного струму (жовтий TT-моторчик із редуктором) для Wokwi.
//
// У Wokwi немає частини «двигун постійного струму» — є тільки крокові. Тому він тут
// кастомним чипом із двома виводами, як у справжнього мотора:
//
//   POS - плюс обмотки, іде на +5V
//   NEG - мінус обмотки, іде на колектор ключа
//
// Вивід NEG піднятий внутрішньою підтяжкою, і це не хитрість, а фізика схеми:
// коли ключ закритий, цей вузол з'єднаний із плюсом через саму обмотку, тобто
// підтягнутий угору. Коли ключ відкритий, він притиснутий до землі.
//
// Чип міряє шпаруватість на NEG (частка часу, коли вузол притиснутий до нуля) і
// крутить ротор тим швидше, чим вона більша. Струмів Wokwi не рахує, тому
// пусковий кидок і зворотна ЕРС тут не моделюються — на платі заради них стоять
// flyback-діод і конденсатор по живленню.
//
// Зібрано без libc (без malloc і printf), щоб лінкувалося як freestanding wasm.

#include "wokwi-api.h"

#define GFX_MAX_PIXELS (80 * 48)  // має збігатися з display у chip.json
#include "chip-gfx.h"

#define COLOR_BG        GFX_RGB(0x0d, 0x11, 0x17)
#define COLOR_FRAME     GFX_RGB(0x1e, 0x25, 0x30)
#define COLOR_HOUSING   GFX_RGB(0x56, 0x5f, 0x6b)
#define COLOR_ROTOR     GFX_RGB(0xfb, 0xbf, 0x24)
#define COLOR_ROTOR_OFF GFX_RGB(0x3a, 0x33, 0x1a)
#define COLOR_TEXT      GFX_RGB(0xe2, 0xe8, 0xf0)
#define COLOR_TEXT_OFF  GFX_RGB(0x47, 0x52, 0x63)

#define FRAME_US    50000  // кадр анімації 20 разів на секунду
#define ROTOR_STEPS 12     // позицій ротора на повний оберт
#define ROTOR_R     13     // радіус корпусу в пікселях

// Зміщення спиці ротора для кожної з ROTOR_STEPS позицій: радіус 11, крок 30°.
// Таблиця замість sin/cos, бо libm тут немає.
static const int8_t ROTOR_DX[ROTOR_STEPS] = {  0,  6,  10, 11,  10,  6,  0, -6, -10, -11, -10, -6 };
static const int8_t ROTOR_DY[ROTOR_STEPS] = { -11, -10, -6,  0,   6, 10, 11, 10,   6,   0,  -6, -10 };

typedef struct {
  pin_t pin_pos;
  pin_t pin_neg;

  uint64_t last_edge_ns;
  uint32_t low_ns;   // накопичено в поточному вікні: ключ відкритий, двигун живиться
  uint32_t high_ns;  // ключ закритий
  uint32_t level;    // останній побачений рівень на NEG

  int duty;   // 0..100 %
  int angle;  // 0..ROTOR_STEPS-1

  gfx_t gfx;
} chip_state_t;

static chip_state_t chip;

static void draw_display(chip_state_t *state) {
  gfx_t *gfx = &state->gfx;

  const int cx = 22;
  const int cy = (int)gfx->height / 2;
  const bool spinning = state->duty > 0;

  gfx_clear(gfx, COLOR_BG);
  gfx_frame(gfx, COLOR_FRAME);

  // Корпус мотора: кільце з двох кіл
  gfx_circle(gfx, cx, cy, ROTOR_R, COLOR_HOUSING);
  gfx_circle(gfx, cx, cy, ROTOR_R - 2, COLOR_BG);

  // Спиця ротора: три точки від центру до краю, щоб було видно напрямок
  const uint32_t rotor = spinning ? COLOR_ROTOR : COLOR_ROTOR_OFF;
  const int dx = ROTOR_DX[state->angle];
  const int dy = ROTOR_DY[state->angle];

  gfx_circle(gfx, cx, cy, 2, rotor);
  gfx_circle(gfx, cx + dx / 3, cy + dy / 3, 1, rotor);
  gfx_circle(gfx, cx + (dx * 2) / 3, cy + (dy * 2) / 3, 1, rotor);
  gfx_circle(gfx, cx + dx, cy + dy, 2, rotor);

  // Шпаруватість цифрами
  const uint32_t text = spinning ? COLOR_TEXT : COLOR_TEXT_OFF;
  const int width = gfx_number(gfx, 42, cy - 5, state->duty, 2, text);
  gfx_glyph(gfx, 42 + width, cy - 5, 10, 2, text);

  gfx_flush(gfx);
}

// Додає час, що минув від попередньої події, до відповідного накопичувача.
static void accumulate(chip_state_t *state, uint64_t now) {
  const uint32_t delta = (uint32_t)(now - state->last_edge_ns);
  if (state->level) {
    state->high_ns += delta;
  } else {
    state->low_ns += delta;
  }
  state->last_edge_ns = now;
}

static void on_pin_change(void *user_data, pin_t pin, uint32_t value) {
  (void)pin;
  chip_state_t *state = (chip_state_t *)user_data;

  accumulate(state, get_sim_nanos());
  state->level = value;
}

static void on_frame(void *user_data) {
  chip_state_t *state = (chip_state_t *)user_data;

  accumulate(state, get_sim_nanos());

  // Двигун живиться, коли NEG притиснутий до нуля, тобто ключ відкритий.
  const uint64_t total = (uint64_t)state->low_ns + (uint64_t)state->high_ns;
  state->duty = total > 0 ? (int)(((uint64_t)state->low_ns * 100) / total) : 0;
  state->low_ns = 0;
  state->high_ns = 0;

  // Швидкість обертання пропорційна шпаруватості: на 100 % це трохи більше
  // третини оберту за кадр, тобто близько восьми обертів на секунду.
  state->angle = (state->angle + (state->duty * 5) / 100) % ROTOR_STEPS;

  draw_display(state);
}

void chip_init(void) {
  chip.pin_pos = pin_init("POS", INPUT);
  // Підтяжка на NEG - це модель самої обмотки: при закритому ключі вузол
  // з'єднаний з плюсом через мідь і стоїть у високому рівні.
  chip.pin_neg = pin_init("NEG", INPUT_PULLUP);

  chip.level = pin_read(chip.pin_neg);
  chip.last_edge_ns = get_sim_nanos();

  gfx_init(&chip.gfx);
  draw_display(&chip);

  const pin_watch_config_t watch = {
    .edge = BOTH,
    .pin_change = on_pin_change,
    .user_data = &chip,
  };
  pin_watch(chip.pin_neg, &watch);

  const timer_config_t timer = {
    .callback = on_frame,
    .user_data = &chip,
  };
  timer_start(timer_init(&timer), FRAME_US, true);
}
