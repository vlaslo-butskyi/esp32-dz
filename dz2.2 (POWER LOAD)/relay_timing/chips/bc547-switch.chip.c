// Транзистор VT1 (BC547B) з креслення ДЗ 1.7. Три виводи, як у справжнього корпусу TO-92:
//
//   B - база, туди приходить сигнал з GPIO через R3
//   C - колектор, він же вхід IN модуля реле, підтягнутий резистором R4 до +5V
//   E - емітер на GND
//
//   База висока -> транзистор відкритий -> колектор притиснутий до землі (низький рівень)
//   База низька -> транзистор закритий -> колектор відпущений, рівень задає підтяжка R4
//
// Wokwi не рахує струмів, тому "відкритий" тут означає активно притиснути колектор до нуля,
// а "закритий" - відпустити пін у високий імпеданс і не заважати підтяжці.
// Емітер у моделі не читається: схема передбачає, що він на землі.
//
// Зібрано без libc (без malloc і printf), щоб лінкувалося як freestanding wasm.

// На дисплеї два індикатори: ліворуч вхід від GPIO, праворуч вихід на реле.
// Між ними кружечок інверсії, як на позначенні логічного НЕ.

#include "wokwi-api.h"

#define GFX_MAX_PIXELS (64 * 24)  // має збігатися з display у chip.json
#include "chip-gfx.h"

#define COLOR_BG       GFX_RGB(0x0d, 0x11, 0x17)
#define COLOR_IN_ON    GFX_RGB(0x4a, 0xde, 0x80)
#define COLOR_IN_OFF   GFX_RGB(0x18, 0x3a, 0x26)
#define COLOR_OUT_ON   GFX_RGB(0x38, 0xbd, 0xf8)
#define COLOR_OUT_OFF  GFX_RGB(0x12, 0x32, 0x40)
#define COLOR_WIRE     GFX_RGB(0x56, 0x5f, 0x6b)

typedef struct {
  pin_t pin_base;
  pin_t pin_collector;
  gfx_t gfx;
} chip_state_t;

static chip_state_t chip;

static void draw_display(chip_state_t *state, uint32_t base_high) {
  gfx_t *gfx = &state->gfx;

  gfx_clear(gfx, COLOR_BG);
  gfx_frame(gfx, GFX_RGB(0x1e, 0x25, 0x30));

  // База: сигнал з GPIO через R3
  gfx_round_rect(gfx, 5, 5, 15, 14, base_high ? COLOR_IN_ON : COLOR_IN_OFF);

  // Стрілка і кружечок інверсії, як на позначенні логічного НЕ
  gfx_arrow(gfx, 22, 12, 12, COLOR_WIRE);
  gfx_circle(gfx, 37, 12, 3, COLOR_WIRE);
  gfx_circle(gfx, 37, 12, 1, COLOR_BG);

  // Колектор: при відкритому транзисторі притиснутий до нуля, інакше відпущений
  gfx_round_rect(gfx, 44, 5, 15, 14, base_high ? COLOR_OUT_OFF : COLOR_OUT_ON);

  gfx_flush(gfx);
}

static void apply_base(chip_state_t *state, uint32_t base_high) {
  if (base_high) {
    // Транзистор відкритий: колектор притиснутий до емітера, тобто до землі
    pin_mode(state->pin_collector, OUTPUT);
    pin_write(state->pin_collector, LOW);
  } else {
    // Транзистор закритий: колектор нічим не керується, рівень задає підтяжка R4
    pin_mode(state->pin_collector, INPUT);
  }

  draw_display(state, base_high);
}

static void chip_pin_change(void *user_data, pin_t pin, uint32_t value) {
  (void)pin;
  apply_base((chip_state_t *)user_data, value);
}

void chip_init(void) {
  chip.pin_base = pin_init("B", INPUT);
  chip.pin_collector = pin_init("C", INPUT);
  pin_init("E", INPUT);  // емітер: у схемі на землі, у моделі не використовується

  gfx_init(&chip.gfx);
  apply_base(&chip, LOW);  // на старті база низька, транзистор закритий

  const pin_watch_config_t config = {
    .edge = BOTH,
    .pin_change = chip_pin_change,
    .user_data = &chip,
  };
  pin_watch(chip.pin_base, &config);
}
