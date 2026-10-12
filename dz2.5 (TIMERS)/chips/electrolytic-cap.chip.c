// Електролітичний конденсатор по живленню для Wokwi.
//
// Частини «конденсатор» у Wokwi немає, тому він тут окремим чипом із двома
// виводами, як у справжнього електроліта:
//
//   POS - плюс, іде на шину живлення двигуна
//   NEG - мінус зі світлою смужкою на корпусі, іде на землю
//
// Wokwi не рахує ані ємності, ані просадок напруги, тому згладжувати цьому
// конденсатору в симуляторі нічого. Він тут із двох причин: щоб схема збігалася
// з платою один в один, і щоб було видно полярність — переплутаний електроліт
// на реальній платі здувається.
//
// Єдине, що чип відстежує, — чи є напруга на шині. Живлення зникло, корпус згас.
//
// Зібрано без libc (без malloc і printf), щоб лінкувалося як freestanding wasm.

#include "wokwi-api.h"

#define GFX_MAX_PIXELS (48 * 40)  // має збігатися з display у chip.json
#include "chip-gfx.h"

#define COLOR_BG       GFX_RGB(0x0d, 0x11, 0x17)
#define COLOR_FRAME    GFX_RGB(0x1e, 0x25, 0x30)
#define COLOR_CAN      GFX_RGB(0x1f, 0x2a, 0x3a)
#define COLOR_CAN_ON   GFX_RGB(0x31, 0x41, 0x5c)
#define COLOR_STRIPE   GFX_RGB(0xcb, 0xd5, 0xe1)
#define COLOR_MINUS    GFX_RGB(0x0d, 0x11, 0x17)
#define COLOR_LEAD     GFX_RGB(0x56, 0x5f, 0x6b)
#define COLOR_TOP      GFX_RGB(0x47, 0x52, 0x63)

#define POLL_US 100000  // перевіряти шину десять разів на секунду

typedef struct {
  pin_t pin_pos;
  pin_t pin_neg;
  bool powered;
  gfx_t gfx;
} chip_state_t;

static chip_state_t chip;

static void draw_display(chip_state_t *state) {
  gfx_t *gfx = &state->gfx;

  const int left = 10;
  const int top = 6;
  const int w = 26;
  const int h = 26;

  gfx_clear(gfx, COLOR_BG);
  gfx_frame(gfx, COLOR_FRAME);

  // Виводи знизу: плюс довший, як на справжньому конденсаторі
  gfx_rect(gfx, left + 6, top + h, 2, 8, COLOR_LEAD);
  gfx_rect(gfx, left + w - 8, top + h, 2, 5, COLOR_LEAD);

  // Корпус і хрестик на торці
  gfx_round_rect(gfx, left, top, w, h, state->powered ? COLOR_CAN_ON : COLOR_CAN);
  gfx_rect(gfx, left + 2, top + 2, w - 4, 2, COLOR_TOP);

  // Смужка мінуса праворуч зі знаком «−» всередині
  gfx_rect(gfx, left + w - 8, top + 4, 6, h - 6, COLOR_STRIPE);
  gfx_rect(gfx, left + w - 7, top + h / 2, 4, 2, COLOR_MINUS);

  gfx_flush(gfx);
}

static void on_poll(void *user_data) {
  chip_state_t *state = (chip_state_t *)user_data;

  const bool powered = pin_read(state->pin_pos) == HIGH;
  if (powered != state->powered) {
    state->powered = powered;
    draw_display(state);
  }
}

void chip_init(void) {
  chip.pin_pos = pin_init("POS", INPUT);
  chip.pin_neg = pin_init("NEG", INPUT);

  chip.powered = pin_read(chip.pin_pos) == HIGH;

  gfx_init(&chip.gfx);
  draw_display(&chip);

  const timer_config_t poll = {
    .callback = on_poll,
    .user_data = &chip,
  };
  timer_start(timer_init(&poll), POLL_US, true);
}
