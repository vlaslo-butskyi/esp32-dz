#include <atomic>
#include <inttypes.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "traffic";

struct Config {
  static constexpr gpio_num_t RED_PIN    = GPIO_NUM_4;
  static constexpr gpio_num_t YELLOW_PIN = GPIO_NUM_5;
  static constexpr gpio_num_t GREEN_PIN  = GPIO_NUM_6;
  static constexpr gpio_num_t BUTTON_PIN = GPIO_NUM_16;

  static constexpr int      PRESSED_LEVEL = 0;   // кнопка на GND, підтяжка внутрішня
  static constexpr uint32_t DEBOUNCE_MS   = 10;  // метод із ДЗ 2.6/timer_debounce

  static constexpr uint32_t TIME_DIVIDER = 1;

  static constexpr uint32_t GREEN_MS       = 10000 / TIME_DIVIDER;
  static constexpr uint32_t GREEN_BLINK_MS =  3000 / TIME_DIVIDER;  // 3 блимання
  static constexpr uint32_t YELLOW_MS      =  3000 / TIME_DIVIDER;
  static constexpr uint32_t RED_MS         = 10000 / TIME_DIVIDER;
  static constexpr uint32_t RED_YELLOW_MS  =  2000 / TIME_DIVIDER;
  static constexpr uint32_t BLINK_HALF_MS  =   500 / TIME_DIVIDER;  // 1 Гц

  static constexpr uint8_t EVENT_QUEUE_LEN = 4;
};

struct Lamp {
  static constexpr uint8_t RED    = 1 << 0;
  static constexpr uint8_t YELLOW = 1 << 1;
  static constexpr uint8_t GREEN  = 1 << 2;
};

enum class Phase : uint8_t {
  Green,        // 8.7.1 дозволяє рух
  GreenBlink,   // 8.7.2 дозволяє рух, попереджає про кінець дозволу
  Yellow,       // 8.7.3 забороняє рух, попереджає про зміну сигналів
  Red,          // 8.7.5 забороняє рух
  RedYellow,    // 8.7.6 забороняє рух, попереджає про наступний зелений
  YellowBlink,  // 8.7.4 дозволяє рух, перехрестя нерегульоване - окремий режим
};
static constexpr uint8_t PHASE_COUNT = 6;

struct PhaseDef {
  Phase       self;         // дублює індекс, щоб перестановку рядків спіймав компілятор
  uint8_t     lamps;        // які лампи задіяні в цій фазі
  uint32_t    blink_ms;     // 0 - горять рівно; інакше півперіод миготіння
  uint32_t    duration_ms;  // 0 - фаза триває до зміни режиму
  Phase       next;
  const char *name;
};

static constexpr PhaseDef TABLE[PHASE_COUNT] = {
  { Phase::Green,       Lamp::GREEN,                 0, Config::GREEN_MS,       Phase::GreenBlink, "GREEN"           },
  { Phase::GreenBlink,  Lamp::GREEN,  Config::BLINK_HALF_MS, Config::GREEN_BLINK_MS, Phase::Yellow,     "GREEN BLINKING"  },
  { Phase::Yellow,      Lamp::YELLOW,                0, Config::YELLOW_MS,      Phase::Red,        "YELLOW"          },
  { Phase::Red,         Lamp::RED,                   0, Config::RED_MS,         Phase::RedYellow,  "RED"             },
  { Phase::RedYellow,   Lamp::RED | Lamp::YELLOW,    0, Config::RED_YELLOW_MS,  Phase::Green,      "RED+YELLOW"      },
  { Phase::YellowBlink, Lamp::YELLOW, Config::BLINK_HALF_MS,                 0, Phase::Yellow,     "YELLOW BLINKING" },
};

constexpr const PhaseDef &def_of(Phase p) { return TABLE[static_cast<uint8_t>(p)]; }

// ───────────────────── перевірки на етапі збірки ─────────────────────
// Послідовність сигналів - це вся умова задачі, тому її перевіряє компілятор,
// а не моя пам'ять: правка, яка ламає ПДР, просто не збереться.

constexpr bool table_matches_enum()
{
    for (uint8_t i = 0; i < PHASE_COUNT; i++) {
        if (static_cast<uint8_t>(TABLE[i].self) != i) return false;
    }
    return true;
}
static_assert(table_matches_enum(), "TABLE rows must stay in the order of enum class Phase");

constexpr bool red_never_with_green()
{
    for (const PhaseDef &d : TABLE) {
        if ((d.lamps & Lamp::RED) && (d.lamps & Lamp::GREEN)) return false;
    }
    return true;
}
static_assert(red_never_with_green(), "no phase may light red and green at the same time");

constexpr bool regulated_cycle_closes()
{
    Phase p = Phase::Green;
    for (uint8_t step = 0; step < 5; step++) {
        p = def_of(p).next;
        if (p == Phase::YellowBlink) return false;  // нічний режим не частина циклу
    }
    return p == Phase::Green;
}
static_assert(regulated_cycle_closes(),
              "the regulated cycle must come back to green in exactly five phases");

static_assert(def_of(Phase::Green).next == Phase::GreenBlink,
              "PDR 8.7.2: green must end with a blinking green warning");
static_assert(def_of(Phase::GreenBlink).next == Phase::Yellow,
              "PDR 8.7.3: the blinking green is followed by yellow");
static_assert(def_of(Phase::Yellow).next == Phase::Red,
              "PDR 8.7.3/8.7.5: yellow is followed by red");
static_assert(def_of(Phase::Red).next == Phase::RedYellow,
              "PDR 8.7.6: red must never switch straight to green");
static_assert(def_of(Phase::RedYellow).next == Phase::Green,
              "PDR 8.7.6: red+yellow announces the green that follows");
static_assert(def_of(Phase::YellowBlink).next == Phase::Yellow,
              "leaving the blinking-yellow mode must go through a steady yellow");

static_assert(def_of(Phase::Yellow).blink_ms == 0,
              "PDR 8.7.3 vs 8.7.4: a steady yellow forbids movement and a blinking one allows it, "
              "so the two are different signals and must not share a phase");
static_assert(def_of(Phase::GreenBlink).blink_ms != 0 &&
              def_of(Phase::GreenBlink).duration_ms % (2 * def_of(Phase::GreenBlink).blink_ms) == 0,
              "the blinking green must end dark, otherwise yellow comes on mid-flash");
static_assert(def_of(Phase::YellowBlink).duration_ms == 0,
              "the blinking-yellow mode lasts until the mode is switched back");
static_assert(Config::BLINK_HALF_MS >= 20,
              "TIME_DIVIDER squeezed the blink below 20 ms: that is no longer a visible blink");

// Такт планувальника. Черга чекає в тіках, тож при 100 Гц кожна подія могла б
// запізнитися на 10 мс - на аналізаторі це видно як розкид фронтів миготіння.
static_assert(CONFIG_FREERTOS_HZ == 1000,
              "set CONFIG_FREERTOS_HZ=1000 in sdkconfig.defaults, the default 100 Hz quantises "
              "every deadline to 10 ms");

// ───────────────────────────── стан ─────────────────────────────

enum class Event : uint8_t { ModeToggle };

static QueueHandle_t      s_events;        // кнопка -> автомат
static esp_timer_handle_t s_settle_timer;  // одноразовий, гасить брязкіт кнопки

static std::atomic<uint32_t> s_isr_edges{0};  // фронтів на лінії кнопки побачив МК

static Phase   s_phase            = Phase::Red;
static int64_t s_phase_started_us = 0;
static int64_t s_blink_toggled_us = 0;
static bool    s_blink_on         = true;
static bool    s_night_wanted     = false;
static uint32_t s_cycles          = 0;

// ─────────────────────────── лампи ───────────────────────────

static void apply_lamps(uint8_t lamps)
{
    // Маски в таблиці перевірені static_assert'ом, але перевірка лишається й у
    // рантаймі: якщо помилка в логіці миготіння колись подасть червоний разом
    // із зеленим, перемагає заборонний сигнал, а не дозвільний.
    if ((lamps & Lamp::RED) && (lamps & Lamp::GREEN)) {
        ESP_LOGE(TAG, "red and green requested together (mask 0x%02x), keeping red only", lamps);
        lamps = static_cast<uint8_t>(lamps & ~Lamp::GREEN);
    }
    gpio_set_level(Config::RED_PIN,    (lamps & Lamp::RED)    ? 1 : 0);
    gpio_set_level(Config::YELLOW_PIN, (lamps & Lamp::YELLOW) ? 1 : 0);
    gpio_set_level(Config::GREEN_PIN,  (lamps & Lamp::GREEN)  ? 1 : 0);
}

// "R.." / ".Y." / "..g" - велика літера горить рівно, мала миготить.
static void lamp_text(const PhaseDef &def, char out[4])
{
    const bool blinking = def.blink_ms != 0;
    out[0] = (def.lamps & Lamp::RED)    ? (blinking ? 'r' : 'R') : '.';
    out[1] = (def.lamps & Lamp::YELLOW) ? (blinking ? 'y' : 'Y') : '.';
    out[2] = (def.lamps & Lamp::GREEN)  ? (blinking ? 'g' : 'G') : '.';
    out[3] = '\0';
}

// ─────────────────────────── автомат ───────────────────────────

static void enter_phase(Phase p)
{
    s_phase            = p;
    s_phase_started_us = esp_timer_get_time();
    s_blink_toggled_us = s_phase_started_us;
    s_blink_on         = true;  // кожна фаза починається зі світла

    const PhaseDef &def = def_of(p);
    apply_lamps(def.lamps);

    char lamps[4];
    lamp_text(def, lamps);
    if (def.duration_ms) {
        ESP_LOGI(TAG, "%-15s [%s] %5" PRIu32 " ms", def.name, lamps, def.duration_ms);
    } else {
        ESP_LOGI(TAG, "%-15s [%s]  until the mode is switched back", def.name, lamps);
    }
}

// Фаза дожила до кінця. Єдиний виняток із таблиці - вхід у нічний режим: він
// дозволений тільки після червоного, бо тоді зміна сигналу ні в кого не
// забирає щойно виданий дозвіл на рух.
static void advance_phase()
{
    if (s_night_wanted && s_phase == Phase::Red) {
        ESP_LOGI(TAG, "mode: entering blinking yellow after red");
        enter_phase(Phase::YellowBlink);
        return;
    }
    const Phase next = def_of(s_phase).next;
    if (next == Phase::Green) {
        // ПДР перелічують цикл із зеленого, тож і лічу його звідти.
        s_cycles++;
        ESP_LOGI(TAG, "cycle %" PRIu32 " starts", s_cycles);
    }
    enter_phase(next);
}

static void on_mode_toggle()
{
    s_night_wanted = !s_night_wanted;
    const uint32_t edges = s_isr_edges.load(std::memory_order_relaxed);

    if (s_phase == Phase::YellowBlink && !s_night_wanted) {
        // Вихід із нічного режиму - через рівний жовтий (ПДР 8.7.3): він
        // забороняє рух і попереджає про зміну сигналів. Далі таблиця сама
        // веде через червоний і червоний+жовтий до зеленого.
        ESP_LOGI(TAG, "mode: leaving blinking yellow through a steady yellow (edges: %" PRIu32 ")",
                 edges);
        enter_phase(def_of(Phase::YellowBlink).next);
        return;
    }

    ESP_LOGI(TAG, "mode: blinking yellow %s, applies after the red phase (edges: %" PRIu32 ")",
             s_night_wanted ? "requested" : "cancelled", edges);
}

// ─────────────────────────── кнопка ───────────────────────────
// Метод із ДЗ 2.6/timer_debounce: ISR нічого не чекає, а перезаводить
// одноразовий esp_timer. Поки контакт брязкотить, таймер не доходить до
// кінця; колбек спрацьовує, коли лінія затихла, і аж тоді рівень піна щось
// означає.

static void IRAM_ATTR button_isr(void *arg)
{
    s_isr_edges.fetch_add(1, std::memory_order_relaxed);
    esp_timer_stop(s_settle_timer);
    esp_timer_start_once(s_settle_timer, Config::DEBOUNCE_MS * 1000ULL);
}

static void settle_timer_cb(void *arg)
{
    static bool pressed = false;

    const bool now_pressed = gpio_get_level(Config::BUTTON_PIN) == Config::PRESSED_LEVEL;
    if (now_pressed == pressed) {
        return;  // брязкіт закінчився там, де й почався
    }
    pressed = now_pressed;
    if (!pressed) {
        return;  // режим перемикає натискання, а не відпускання
    }

    const Event ev = Event::ModeToggle;
    xQueueSend(s_events, &ev, 0);
}

// ─────────────────────────── ініціалізація ───────────────────────────

static void configure_gpio()
{
    const gpio_config_t lamps = {
        .pin_bit_mask = (1ULL << Config::RED_PIN) | (1ULL << Config::YELLOW_PIN) |
                        (1ULL << Config::GREEN_PIN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&lamps));
    apply_lamps(0);

    const gpio_config_t button = {
        .pin_bit_mask = 1ULL << Config::BUTTON_PIN,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_ANYEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&button));
}

// Черга чекає в тіках, а дедлайни я тримаю в мікросекундах esp_timer.
// Округлення вгору і мінімум один тік - щоб нульовий таймаут не перетворив
// очікування на холостий цикл.
static TickType_t wait_ticks(int64_t until_us, int64_t now_us)
{
    if (until_us == INT64_MAX) {
        return portMAX_DELAY;
    }
    const int64_t left_us = until_us - now_us;
    if (left_us <= 0) {
        return 0;
    }
    const TickType_t ticks = pdMS_TO_TICKS((left_us + 999) / 1000);
    return ticks ? ticks : 1;
}

extern "C" void app_main()
{
    s_events = xQueueCreate(Config::EVENT_QUEUE_LEN, sizeof(Event));
    ESP_ERROR_CHECK(s_events != nullptr ? ESP_OK : ESP_ERR_NO_MEM);

    // Поля перелічені всі й у порядку оголошення: C пропущені члени прощав,
    // а C++ під -Wextra дає -Wmissing-field-initializers.
    const esp_timer_create_args_t settle_args = {
        .callback              = settle_timer_cb,
        .arg                   = nullptr,
        .dispatch_method       = ESP_TIMER_TASK,
        .name                  = "settle",
        .skip_unhandled_events = false,
    };
    ESP_ERROR_CHECK(esp_timer_create(&settle_args, &s_settle_timer));

    configure_gpio();
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(Config::BUTTON_PIN, button_isr, nullptr));

    ESP_LOGI(TAG, "ready: PDR cycle %" PRIu32 "+%" PRIu32 "+%" PRIu32 "+%" PRIu32 "+%" PRIu32
                  " ms, blink half-period %" PRIu32 " ms, button GPIO %d toggles blinking yellow",
             Config::GREEN_MS, Config::GREEN_BLINK_MS, Config::YELLOW_MS, Config::RED_MS,
             Config::RED_YELLOW_MS, Config::BLINK_HALF_MS, Config::BUTTON_PIN);

    // Старт із червоного: єдина фаза, яку можна ввімкнути на вже зайнятому
    // перехресті, нікому не збрехавши. Далі червоний+жовтий попередить про
    // зелений, як і в звичайному циклі.
    enter_phase(Phase::Red);

    while (true) {
        const int64_t   now = esp_timer_get_time();
        const PhaseDef &def = def_of(s_phase);

        const int64_t phase_end_us =
            def.duration_ms ? s_phase_started_us + def.duration_ms * 1000LL : INT64_MAX;
        const int64_t blink_at_us =
            def.blink_ms ? s_blink_toggled_us + def.blink_ms * 1000LL : INT64_MAX;

        if (now >= phase_end_us) {
            advance_phase();
            continue;
        }
        if (now >= blink_at_us) {
            s_blink_on = !s_blink_on;
            // Якір на дедлайн, а не на now: інакше сітка миготіння дрейфувала б
            // на час пробудження задачі.
            s_blink_toggled_us = blink_at_us;
            apply_lamps(s_blink_on ? def.lamps : 0);
            continue;
        }

        const int64_t next_us = phase_end_us < blink_at_us ? phase_end_us : blink_at_us;
        Event         ev;
        if (xQueueReceive(s_events, &ev, wait_ticks(next_us, now)) == pdTRUE) {
            on_mode_toggle();
        }
    }
}
