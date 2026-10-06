// Керамічний конденсатор 100 нФ для Wokwi.
//
// Частини «конденсатор» у Wokwi немає, тому він тут окремим чипом із двома
// виводами — рівно стільки, скільки в деталі на платі. Кераміка неполярна,
// тож виводи так і називаються: 1 і 2.
//
// Чип нічого не фільтрує і не може фільтрувати. Wokwi рахує логічні рівні,
// а конденсатор на платі — це шунт на землю: щоб вносити затримку фронту,
// елемент мусив би стояти в розрив лінії й мати вхід та вихід, тобто вже не
// був би конденсатором. Тому в симуляторі RC-варіанти поводяться так само,
// як без фільтра, а вся різниця міряється на залізі логічним аналізатором.
//
// Сенс чипа в тому, щоб схема в Wokwi збігалася з платою один в один і було
// видно, куди саме ввімкнено 100 нФ. Єдине, що він робить, — підсвічує корпус
// за рівнем на виводі 1, щоб на схемі було видно стан вузла.
//
// Зібрано без libc (без malloc і printf), щоб лінкувалося як freestanding wasm.

#include "wokwi-api.h"

#define GFX_MAX_PIXELS (48 * 40)  // має збігатися з display у chip.json
#include "chip-gfx.h"

#define COLOR_BG     GFX_RGB(0x0d, 0x11, 0x17)
#define COLOR_FRAME  GFX_RGB(0x1e, 0x25, 0x30)
#define COLOR_BODY   GFX_RGB(0x8a, 0x6d, 0x2f)
#define COLOR_BODY_H GFX_RGB(0xc2, 0x99, 0x3f)
#define COLOR_LEAD   GFX_RGB(0x56, 0x5f, 0x6b)
#define COLOR_TEXT   GFX_RGB(0x0d, 0x11, 0x17)

#define POLL_US 50000  // перемальовувати двадцять разів на секунду

typedef struct {
  pin_t pin_1;
  pin_t pin_2;
  bool  high;
  gfx_t gfx;
} chip_state_t;

static chip_state_t chip;

static void draw_display(chip_state_t *state) {
  gfx_t *gfx = &state->gfx;

  gfx_clear(gfx, COLOR_BG);
  gfx_frame(gfx, COLOR_FRAME);

  // Дві ніжки знизу, однакової довжини: кераміка неполярна
  gfx_rect(gfx, 15, 26, 2, 10, COLOR_LEAD);
  gfx_rect(gfx, 31, 26, 2, 10, COLOR_LEAD);

  // Корпус-таблетка
  gfx_round_rect(gfx, 9, 6, 30, 21, state->high ? COLOR_BODY_H : COLOR_BODY);

  // Маркування 104 = 10 * 10^4 пФ = 100 нФ
  gfx_number(gfx, 14, 12, 104, 1, COLOR_TEXT);

  gfx_flush(gfx);
}

static void on_poll(void *user_data) {
  chip_state_t *state = (chip_state_t *)user_data;

  const bool high = pin_read(state->pin_1) == HIGH;
  if (high != state->high) {
    state->high = high;
    draw_display(state);
  }
}

void chip_init(void) {
  // Обидва виводи — входи: чип тільки спостерігає за вузлом і нічого в нього не жене.
  chip.pin_1 = pin_init("1", INPUT);
  chip.pin_2 = pin_init("2", INPUT);

  chip.high = pin_read(chip.pin_1) == HIGH;

  gfx_init(&chip.gfx);
  draw_display(&chip);

  const timer_config_t poll = {
    .callback = on_poll,
    .user_data = &chip,
  };
  timer_start(timer_init(&poll), POLL_US, true);
}
