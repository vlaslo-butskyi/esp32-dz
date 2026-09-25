// Ключ VT1 (2N2222A) для ДЗ 2.2. Три виводи, як у справжнього корпусу TO-92:
//
//   B - база, туди приходить ШІМ з GPIO через R 220 Ом
//   C - колектор, до нього підключений мінус навантаження
//   E - емітер на GND
//
//   База висока -> транзистор відкритий -> колектор притиснутий до землі, навантаження живиться
//   База низька -> транзистор закритий -> колектор відпущений, струм через навантаження не тече
//
// Wokwi не рахує струмів, тому "відкритий" тут означає активно притиснути колектор до нуля,
// а "закритий" - відпустити пін у високий імпеданс. Через це в симуляторі не видно ані
// пускового кидка струму двигуна, ані зворотної ЕРС при закритті ключа: заради них
// у схемі на платі стоять flyback-діод і конденсатор по живленню.
// Емітер у моделі не читається: схема передбачає, що він на землі.
//
// Модель та сама, що й для BC547B у ДЗ 1.7 і в relay_timing, - різниця між цими
// транзисторами суто струмова (100 мА проти 800 мА), а логіка ключа однакова.
//
// Зібрано без libc (без malloc і printf), щоб лінкувалося як freestanding wasm.

// На дисплеї два індикатори: ліворуч сигнал від GPIO, праворуч стан навантаження.
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

  // База: сигнал з GPIO через R
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
    // Транзистор закритий: колектор нічим не керується, струм через навантаження не тече
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
