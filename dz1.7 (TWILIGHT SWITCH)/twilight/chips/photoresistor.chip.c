// Фоторезистор R1 з креслення ДЗ 1.7, окрема деталь на два виводи:
//
//   A -> +3.3V,  B -> вузол дільника (GPIO 1 і резистор R2 на GND)
//
// Повзунок "Light, %" грає роль освітлення: 0 = темрява, 100 = яскраве світло.
// Опір береться з таблиці (модель GL5528: ~500 кОм у темряві, ~1 кОм на
// яскравому світлі), проміжні значення інтерполюються лінійно.
//
// Wokwi не розвʼязує кола з резисторів: кожна деталь лише читає або задає рівень
// на своєму піні. Тому напругу вузла рахує цей чип, а номінал нижнього плеча
// береться з атрибута "r2" (кОм) у diagram.json - він має збігатися з номіналом
// резистора, який стоїть поруч на схемі.
//
// Голого фоторезистора в бібліотеці Wokwi немає, а модуль wokwi-photoresistor-sensor
// має власний вбудований дільник з дзеркальною полярністю.
//
// На дисплеї: поточне освітлення у відсотках, смужка з двома позначками порогів
// і кольорова крапка стану - зелена коли прошивка має вмикати реле, бурштинова
// коли вимикати, сіра в зоні гістерезису.
//
// Зібрано без libc, тому замість pow() - таблиця з 11 точок.

#include "wokwi-api.h"

#define GFX_MAX_PIXELS (96 * 40)  // має збігатися з display у chip.json
#include "chip-gfx.h"

#define VCC_FALLBACK_VOLTS 3.3f
#define ADC_MAX 4095.0f
#define UPDATE_PERIOD_US 50000  // 20 разів на секунду

// Мають збігатися з THRESHOLD_DARK / THRESHOLD_LIGHT у main.cpp
#define THRESHOLD_DARK_ADC 1200
#define THRESHOLD_LIGHT_ADC 1800

// Опір LDR, кОм, для освітлення 0, 10, 20 ... 100 %
static const float LDR_KOHM[11] = {
  500.0f, 268.7f, 144.3f, 77.45f, 41.6f, 22.34f, 12.0f, 6.44f, 3.46f, 1.862f, 1.0f
};

#define COLOR_BG      GFX_RGB(0x0d, 0x11, 0x17)
#define COLOR_TRACK   GFX_RGB(0x1b, 0x20, 0x29)
#define COLOR_DARK    GFX_RGB(0x1b, 0x2a, 0x4a)
#define COLOR_BRIGHT  GFX_RGB(0xff, 0xd2, 0x4a)
#define COLOR_TEXT    GFX_RGB(0xe6, 0xed, 0xf3)
#define COLOR_ON      GFX_RGB(0x4a, 0xde, 0x80)
#define COLOR_OFF     GFX_RGB(0xf5, 0x9e, 0x0b)
#define COLOR_IDLE    GFX_RGB(0x56, 0x5f, 0x6b)

typedef struct {
  pin_t pin_a;   // верхній вивід, на +3.3V
  pin_t pin_b;   // нижній вивід: вузол дільника
  uint32_t attr_light;
  float r2_ohms;
  float vcc_volts;
  gfx_t gfx;
  int tick_dark;   // позиція порога "темно" на смужці, у відсотках освітлення
  int tick_light;  // позиція порога "світло"
} chip_state_t;

static chip_state_t chip;

static float ldr_ohms(float light_percent) {
  if (light_percent < 0.0f) light_percent = 0.0f;
  if (light_percent > 100.0f) light_percent = 100.0f;

  const float position = light_percent / 10.0f;
  int index = (int)position;
  if (index > 9) index = 9;

  const float fraction = position - (float)index;
  const float kohm = LDR_KOHM[index] + (LDR_KOHM[index + 1] - LDR_KOHM[index]) * fraction;

  return kohm * 1000.0f;
}

static float node_voltage(const chip_state_t *state, float light_percent) {
  const float r_ldr = ldr_ohms(light_percent);
  return state->vcc_volts * state->r2_ohms / (r_ldr + state->r2_ohms);
}

static int adc_code(const chip_state_t *state, float light_percent) {
  return (int)(node_voltage(state, light_percent) / state->vcc_volts * ADC_MAX);
}

// Шукає освітлення, при якому АЦП перетинає заданий код
static int light_for_adc(const chip_state_t *state, int target_adc) {
  for (int percent = 0; percent <= 100; percent++) {
    if (adc_code(state, (float)percent) >= target_adc) {
      return percent;
    }
  }
  return 100;
}

static void draw_display(chip_state_t *state, int light_percent) {
  gfx_t *gfx = &state->gfx;
  const int adc = adc_code(state, (float)light_percent);

  gfx_clear(gfx, COLOR_BG);
  gfx_frame(gfx, GFX_RGB(0x1e, 0x25, 0x30));

  // Освітлення у відсотках
  gfx_number(gfx, 7, 7, light_percent, 3, COLOR_TEXT);

  // Сонечко, яскравість іде за повзунком
  gfx_sun(gfx, 55, 14, gfx_mix(GFX_RGB(0x2a, 0x31, 0x3c), COLOR_BRIGHT, light_percent * 256 / 100));

  // Крапка стану: що прошивка має зробити з реле при такому освітленні
  uint32_t state_color = COLOR_IDLE;
  if (adc < THRESHOLD_DARK_ADC) state_color = COLOR_ON;
  if (adc > THRESHOLD_LIGHT_ADC) state_color = COLOR_OFF;
  gfx_circle(gfx, 82, 14, 7, gfx_mix(state_color, COLOR_BG, 190));
  gfx_circle(gfx, 82, 14, 4, state_color);

  // Смужка освітлення
  const int bar_x = 7;
  const int bar_y = 27;
  const int bar_w = (int)gfx->width - 14;
  const int bar_h = 8;

  const int tick_dark_x = bar_x + bar_w * state->tick_dark / 100;
  const int tick_light_x = bar_x + bar_w * state->tick_light / 100;

  // Підкладка: ліва зона "реле вмикається", права "вимикається"
  gfx_round_rect(gfx, bar_x, bar_y, bar_w, bar_h, COLOR_TRACK);
  gfx_rect(gfx, bar_x, bar_y, tick_dark_x - bar_x, bar_h, gfx_mix(COLOR_TRACK, COLOR_ON, 40));
  gfx_rect(gfx, tick_light_x, bar_y, bar_x + bar_w - tick_light_x, bar_h, gfx_mix(COLOR_TRACK, COLOR_OFF, 40));

  // Заповнення за поточним освітленням
  const int fill_w = bar_w * light_percent / 100;
  const uint32_t fill_color = gfx_mix(COLOR_DARK, COLOR_BRIGHT, light_percent * 256 / 100);
  gfx_round_rect(gfx, bar_x, bar_y, fill_w, bar_h, fill_color);

  // Позначки порогів з main.cpp
  gfx_rect(gfx, tick_dark_x, bar_y - 3, 2, bar_h + 6, COLOR_ON);
  gfx_rect(gfx, tick_light_x, bar_y - 3, 2, bar_h + 6, COLOR_OFF);

  gfx_flush(gfx);
}

static void chip_timer_event(void *user_data) {
  chip_state_t *state = (chip_state_t *)user_data;

  const float light = attr_read_float(state->attr_light);

  // Напруга живлення береться з верхнього виводу, якщо симулятор її показує
  const float supply = pin_adc_read(state->pin_a);
  state->vcc_volts = (supply > 2.0f && supply < 5.5f) ? supply : VCC_FALLBACK_VOLTS;

  pin_dac_write(state->pin_b, node_voltage(state, light));

  draw_display(state, (int)(light + 0.5f));
}

void chip_init(void) {
  chip.pin_a = pin_init("A", ANALOG);
  chip.pin_b = pin_init("B", ANALOG);

  chip.attr_light = attr_init_float("light", 70.0f);
  chip.vcc_volts = VCC_FALLBACK_VOLTS;

  // Номінал нижнього плеча задається в diagram.json у кОм і має збігатися
  // з резистором, який стоїть поруч на схемі
  const uint32_t attr_r2 = attr_init_float("r2", 10.0f);
  chip.r2_ohms = attr_read_float(attr_r2) * 1000.0f;

  // Позначки порогів на шкалі рахуються вже з урахуванням R2
  chip.tick_dark = light_for_adc(&chip, THRESHOLD_DARK_ADC);
  chip.tick_light = light_for_adc(&chip, THRESHOLD_LIGHT_ADC);

  gfx_init(&chip.gfx);
  draw_display(&chip, 70);

  const timer_config_t timer_config = {
    .user_data = &chip,
    .callback = chip_timer_event,
  };

  timer_t timer = timer_init(&timer_config);
  timer_start(timer, UPDATE_PERIOD_US, true);
}
