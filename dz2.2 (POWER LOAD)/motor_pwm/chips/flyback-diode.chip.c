// Захисний (flyback) діод 1N4007 для Wokwi.
//
// Частини «діод» у Wokwi немає, тому він тут окремим чипом із двома виводами,
// як у корпусі DO-41:
//
//   A - анод, іде на мінус двигуна (той самий вузол, що й колектор ключа)
//   K - катод зі смужкою, іде на плюс живлення
//
// Wokwi не рахує ані струмів, ані аналогових напруг, тому справжнього сплеску
// зворотної ЕРС тут немає і згасити його нічим. Що чип справді робить — ловить
// момент, у який реальний діод відкривається: фронт угору на аноді, тобто
// закриття транзисторного ключа. Поки ШІМ працює, індикатор горить, бо діод
// відкривається на кожному періоді. Зупинився ШІМ - згас і він.
//
// Зібрано без libc (без malloc і printf), щоб лінкувалося як freestanding wasm.

#include "wokwi-api.h"

#define GFX_MAX_PIXELS (64 * 32)  // має збігатися з display у chip.json
#include "chip-gfx.h"

#define COLOR_BG        GFX_RGB(0x0d, 0x11, 0x17)
#define COLOR_FRAME     GFX_RGB(0x1e, 0x25, 0x30)
#define COLOR_BODY      GFX_RGB(0x2b, 0x33, 0x40)
#define COLOR_BODY_ON   GFX_RGB(0x38, 0xbd, 0xf8)
#define COLOR_STRIPE    GFX_RGB(0xe2, 0xe8, 0xf0)
#define COLOR_LEAD      GFX_RGB(0x56, 0x5f, 0x6b)

#define IDLE_US 100000  // якщо 100 мс не було фронтів, ШІМ зупинився

typedef struct {
  pin_t pin_anode;
  pin_t pin_cathode;
  timer_t idle_timer;
  bool conducting;
  gfx_t gfx;
} chip_state_t;

static chip_state_t chip;

static void draw_display(chip_state_t *state) {
  gfx_t *gfx = &state->gfx;

  const int cy = (int)gfx->height / 2;
  const uint32_t body = state->conducting ? COLOR_BODY_ON : COLOR_BODY;

  gfx_clear(gfx, COLOR_BG);
  gfx_frame(gfx, COLOR_FRAME);

  // Виводи
  gfx_rect(gfx, 4, cy - 1, 14, 2, COLOR_LEAD);
  gfx_rect(gfx, 46, cy - 1, 14, 2, COLOR_LEAD);

  // Корпус: анод ліворуч, катодна смужка праворуч, як на справжньому 1N4007
  gfx_round_rect(gfx, 18, cy - 7, 28, 14, body);
  gfx_rect(gfx, 40, cy - 7, 4, 14, COLOR_STRIPE);

  gfx_flush(gfx);
}

static void on_idle(void *user_data) {
  chip_state_t *state = (chip_state_t *)user_data;

  if (state->conducting) {
    state->conducting = false;
    draw_display(state);
  }
}

static void on_anode_rise(void *user_data, pin_t pin, uint32_t value) {
  (void)pin;
  (void)value;
  chip_state_t *state = (chip_state_t *)user_data;

  // Ключ щойно закрився: струм обмотки нікуди дітися не може і йде через діод.
  if (!state->conducting) {
    state->conducting = true;
    draw_display(state);
  }

  timer_start(state->idle_timer, IDLE_US, false);
}

void chip_init(void) {
  chip.pin_anode = pin_init("A", INPUT);
  chip.pin_cathode = pin_init("K", INPUT);

  gfx_init(&chip.gfx);
  draw_display(&chip);

  const timer_config_t idle = {
    .callback = on_idle,
    .user_data = &chip,
  };
  chip.idle_timer = timer_init(&idle);

  const pin_watch_config_t watch = {
    .edge = RISING,
    .pin_change = on_anode_rise,
    .user_data = &chip,
  };
  pin_watch(chip.pin_anode, &watch);
}
