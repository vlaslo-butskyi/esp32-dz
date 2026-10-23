#include <atomic>
#include <inttypes.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "barrier";

struct Config {
  static constexpr gpio_num_t RED_PIN    = GPIO_NUM_4;
  static constexpr gpio_num_t YELLOW_PIN = GPIO_NUM_5;
  static constexpr gpio_num_t GREEN_PIN  = GPIO_NUM_6;
  static constexpr gpio_num_t BUTTON_PIN = GPIO_NUM_16;

  static constexpr int      PRESSED_LEVEL = 0;
  static constexpr uint32_t DEBOUNCE_MS   = 10;

  static constexpr uint32_t BARRIER_TRAVEL_MS = 600;

  static constexpr uint32_t LEDC_MARGIN_MS = 40;
  static constexpr uint32_t BARRIER_BUDGET_MS = BARRIER_TRAVEL_MS + LEDC_MARGIN_MS;

  static constexpr uint32_t HOLD_FALLBACK_MS = 1000;

  static constexpr uint32_t TIME_DIVIDER = 1;

  static constexpr uint32_t GREEN_MS       = 10000 / TIME_DIVIDER;
  static constexpr uint32_t GREEN_BLINK_MS =  3000 / TIME_DIVIDER;
  static constexpr uint32_t YELLOW_MS      =  3000 / TIME_DIVIDER;
  static constexpr uint32_t RED_MS         = 10000 / TIME_DIVIDER;
  static constexpr uint32_t RED_YELLOW_MS  =  2000 / TIME_DIVIDER;
  static constexpr uint32_t BLINK_HALF_MS  =   500 / TIME_DIVIDER;

  static constexpr uint8_t EVENT_QUEUE_LEN = 8;
};

struct ServoCfg {
  static constexpr gpio_num_t       PIN        = GPIO_NUM_8;
  static constexpr ledc_mode_t      MODE       = LEDC_LOW_SPEED_MODE;
  static constexpr ledc_timer_t     TIMER      = LEDC_TIMER_0;
  static constexpr ledc_channel_t   CHANNEL    = LEDC_CHANNEL_0;
  static constexpr ledc_timer_bit_t RESOLUTION = LEDC_TIMER_14_BIT;

  static constexpr uint32_t FREQ_HZ   = 50;
  static constexpr uint32_t PERIOD_US = 1000000 / FREQ_HZ;
  static constexpr uint32_t STEPS     = 1u << static_cast<uint32_t>(RESOLUTION);

  static constexpr uint32_t DOWN_US = 1000;  // стріла перекриває проїзд
  static constexpr uint32_t UP_US   = 2000;  // стріла вгорі
};

struct Lamp {
  static constexpr uint8_t RED    = 1 << 0;
  static constexpr uint8_t YELLOW = 1 << 1;
  static constexpr uint8_t GREEN  = 1 << 2;
};

enum class Phase : uint8_t {
  Green,        // 8.7.1
  GreenBlink,   // 8.7.2
  Yellow,       // 8.7.3
  Red,          // 8.7.5
  RedYellow,    // 8.7.6
  YellowBlink,  // 8.7.4 - окремий режим
};
static constexpr uint8_t PHASE_COUNT = 6;

struct PhaseDef {
  Phase       self;
  uint8_t     lamps;
  uint32_t    blink_ms;
  uint32_t    duration_ms;
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
// Ті самі, що в traffic_light - порядок сигналів перевіряє компілятор.

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
        if (p == Phase::YellowBlink) return false;
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
static_assert(CONFIG_FREERTOS_HZ == 1000,
              "set CONFIG_FREERTOS_HZ=1000 in sdkconfig.defaults, the default 100 Hz quantises "
              "every deadline to 10 ms");

// ─── і два нових: тут перевіряється вже не таблиця, а механіка ───

static_assert(Config::YELLOW_MS >= Config::BARRIER_BUDGET_MS,
              "the yellow phase is shorter than the boom travel budget: the barrier would still "
              "be moving when the red phase starts");
static_assert(Config::RED_YELLOW_MS >= Config::BARRIER_BUDGET_MS,
              "the red+yellow phase is shorter than the boom travel budget: green would come on "
              "before the barrier is up");
static_assert(Config::LEDC_MARGIN_MS >= 2 * (1000 / ServoCfg::FREQ_HZ),
              "the margin must cover at least two PWM periods: ledc_update_duty() only lands on "
              "the next period boundary");
static_assert(Config::HOLD_FALLBACK_MS >= 100 && Config::HOLD_FALLBACK_MS <= 5000,
              "the hold fallback slice belongs between 100 ms and 5 s: shorter floods the log, "
              "longer stalls the cycle");

static constexpr uint32_t SERVO_STEP_NS = (ServoCfg::PERIOD_US * 1000) / ServoCfg::STEPS;
static_assert(SERVO_STEP_NS <= 2000,
              "servo duty step is coarser than 2 us: the SG90 dead band is around 5 us, so the "
              "angle would quantise visibly");
static_assert(ServoCfg::DOWN_US >= 500 && ServoCfg::UP_US <= 2500 &&
              ServoCfg::DOWN_US != ServoCfg::UP_US,
              "servo pulses must stay inside the 500-2500 us SG90 range and differ from each other");

// ───────────────────────────── стан ─────────────────────────────

enum class Event : uint8_t { ModeToggle };

enum class Barrier : uint8_t { Unknown, Up, Lowering, Down, Raising };

static const char *barrier_name(Barrier b)
{
    switch (b) {
    case Barrier::Unknown:  return "unknown";
    case Barrier::Up:       return "up";
    case Barrier::Lowering: return "lowering";
    case Barrier::Down:     return "down";
    case Barrier::Raising:  return "raising";
    }
    return "?";
}

static QueueHandle_t      s_events;
static esp_timer_handle_t s_settle_timer;

static std::atomic<uint32_t> s_isr_edges{0};

static Phase    s_phase            = Phase::Red;
static int64_t  s_phase_started_us = 0;
static int64_t  s_blink_toggled_us = 0;
static bool     s_blink_on         = true;
static bool     s_night_wanted     = false;
static uint32_t s_cycles           = 0;

static Barrier  s_barrier             = Barrier::Unknown;
static int64_t  s_barrier_started_us  = 0;
static int64_t  s_barrier_deadline_us = INT64_MAX;
static uint32_t s_pulse_us            = 0;
static uint32_t s_interlock_holds     = 0;

// ─────────────────────────── серво ───────────────────────────
// Структури драйвера LEDC заповнюю не списком усіх полів, як у ДЗ 2.6, а
// через `= {}` і присвоєння: склад ledc_timer_config_t/ledc_channel_config_t
// між версіями IDF міняється (у 5.4+ додалися deconfigure і sleep_mode), а ця
// прошивка має збиратися і на 5.5.5, і на 6.1. `= {}` при цьому не дає
// -Wmissing-field-initializers - gcc звільняє саме цю форму.

static void servo_init()
{
    ledc_timer_config_t timer = {};
    timer.speed_mode      = ServoCfg::MODE;
    timer.duty_resolution = ServoCfg::RESOLUTION;
    timer.timer_num       = ServoCfg::TIMER;
    timer.freq_hz         = ServoCfg::FREQ_HZ;
    timer.clk_cfg         = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    ledc_channel_config_t channel = {};
    channel.gpio_num   = ServoCfg::PIN;
    channel.speed_mode = ServoCfg::MODE;
    channel.channel    = ServoCfg::CHANNEL;
    channel.intr_type  = LEDC_INTR_DISABLE;
    channel.timer_sel  = ServoCfg::TIMER;
    channel.duty       = 0;
    channel.hpoint     = 0;
    ESP_ERROR_CHECK(ledc_channel_config(&channel));
}

// PWM не знімається на припаркованій стрілі. Спокуса є - ledc_stop() прибрав
// би гудіння і струм утримання, - але без механічного упору стріла тоді просто
// обвисає, а шлагбаум, який сам відкривається, гірший за той, що гуде.
static void servo_write(uint32_t pulse_us)
{
    const uint32_t duty = (pulse_us * ServoCfg::STEPS) / ServoCfg::PERIOD_US;
    ESP_ERROR_CHECK(ledc_set_duty(ServoCfg::MODE, ServoCfg::CHANNEL, duty));
    ESP_ERROR_CHECK(ledc_update_duty(ServoCfg::MODE, ServoCfg::CHANNEL));
    s_pulse_us = pulse_us;
}

// ─────────────────────────── лампи ───────────────────────────

static void apply_lamps(uint8_t lamps)
{
    if ((lamps & Lamp::RED) && (lamps & Lamp::GREEN)) {
        ESP_LOGE(TAG, "red and green requested together (mask 0x%02x), keeping red only", lamps);
        lamps = static_cast<uint8_t>(lamps & ~Lamp::GREEN);
    }
    gpio_set_level(Config::RED_PIN,    (lamps & Lamp::RED)    ? 1 : 0);
    gpio_set_level(Config::YELLOW_PIN, (lamps & Lamp::YELLOW) ? 1 : 0);
    gpio_set_level(Config::GREEN_PIN,  (lamps & Lamp::GREEN)  ? 1 : 0);
}

static void lamp_text(const PhaseDef &def, char out[4])
{
    const bool blinking = def.blink_ms != 0;
    out[0] = (def.lamps & Lamp::RED)    ? (blinking ? 'r' : 'R') : '.';
    out[1] = (def.lamps & Lamp::YELLOW) ? (blinking ? 'y' : 'Y') : '.';
    out[2] = (def.lamps & Lamp::GREEN)  ? (blinking ? 'g' : 'G') : '.';
    out[3] = '\0';
}

// ─────────────────────────── шлагбаум ───────────────────────────

static void barrier_move(Barrier moving, uint32_t pulse_us)
{
    servo_write(pulse_us);
    s_barrier             = moving;
    s_barrier_started_us  = esp_timer_get_time();
    s_barrier_deadline_us = s_barrier_started_us + Config::BARRIER_BUDGET_MS * 1000LL;
    ESP_LOGI(TAG, "boom: %s, pulse %" PRIu32 " us, budget %" PRIu32 " ms",
             barrier_name(moving), pulse_us, Config::BARRIER_BUDGET_MS);
}

static void barrier_lower()
{
    if (s_barrier == Barrier::Down || s_barrier == Barrier::Lowering) return;
    barrier_move(Barrier::Lowering, ServoCfg::DOWN_US);
}

static void barrier_raise()
{
    if (s_barrier == Barrier::Up || s_barrier == Barrier::Raising) return;
    barrier_move(Barrier::Raising, ServoCfg::UP_US);
}

// Бюджет ходу вийшов. Зворотного зв'язку немає, тож це і є «доїхала»: далі
// прошивка оперує часом, а не положенням.
static void barrier_budget_expired()
{
    const int64_t took_ms = (esp_timer_get_time() - s_barrier_started_us) / 1000;
    s_barrier_deadline_us = INT64_MAX;

    if (s_barrier == Barrier::Lowering) {
        s_barrier = Barrier::Down;
        ESP_LOGI(TAG, "boom down (%" PRId64 " ms, by budget)", took_ms);
        return;
    }
    if (s_barrier == Barrier::Raising) {
        s_barrier = Barrier::Up;
        ESP_LOGI(TAG, "boom up (%" PRId64 " ms, by budget)", took_ms);
    }
}

// ─────────────────────────── автомат ───────────────────────────

static void enter_phase(Phase p)
{
    s_phase            = p;
    s_phase_started_us = esp_timer_get_time();
    s_blink_toggled_us = s_phase_started_us;
    s_blink_on         = true;

    const PhaseDef &def = def_of(p);
    apply_lamps(def.lamps);

    char lamps[4];
    lamp_text(def, lamps);
    if (def.duration_ms) {
        ESP_LOGI(TAG, "%-15s [%s] %5" PRIu32 " ms  boom %s", def.name, lamps, def.duration_ms,
                 barrier_name(s_barrier));
    } else {
        ESP_LOGI(TAG, "%-15s [%s]  until the mode is switched back  boom %s", def.name, lamps,
                 barrier_name(s_barrier));
    }

    // Команди стрілі прив'язані до фаз, а не до таймерів: заборонний сигнал
    // опускає, попередження про зелений піднімає.
    switch (p) {
    case Phase::Yellow:      barrier_lower(); break;
    case Phase::RedYellow:   barrier_raise(); break;
    case Phase::YellowBlink: barrier_raise(); break;
    default: break;
    }
}

// Продовжити поточну фазу, не чіпаючи ламп: автомат не бреше про сигнал, він
// просто не йде далі, поки механіка не готова. Якщо час доїзду відомий -
// продовжую рівно на нього, щоб у лог пішов один рядок, а не тридцять.
static void hold_phase(const char *why)
{
    const int64_t now         = esp_timer_get_time();
    const int64_t duration_us = def_of(s_phase).duration_ms * 1000LL;

    // Кінець фази рахую НАПРЯМУ, а не додаю шматок до старту. Різниця не
    // теоретична: на момент тримання фаза вже прострочена - між її дедлайном
    // і цим викликом устигають пройти і команда серво, і рядок у UART, - і
    // якщо додавати до старту, це прострочення з'їдає запас. Перший прогон на
    // залізі саме так і виглядав: фаза закінчувалася за 1 мс до дедлайну
    // стріли, і тримання спрацьовувало вдруге ще на мілісекунду.
    const int64_t target_end_us =
        (s_barrier_deadline_us != INT64_MAX && s_barrier_deadline_us > now)
            ? s_barrier_deadline_us + 1000
            : now + Config::HOLD_FALLBACK_MS * 1000LL;

    const int64_t extra_us = target_end_us - (s_phase_started_us + duration_us);
    s_phase_started_us     = target_end_us - duration_us;
    s_interlock_holds++;
    ESP_LOGW(TAG, "interlock: %s (boom %s), holding %s %" PRId64 " ms more (hold %" PRIu32 ")",
             why, barrier_name(s_barrier), def_of(s_phase).name, extra_us / 1000,
             s_interlock_holds);
}

static void advance_phase()
{
    // Вхід у нічний режим: стріла має бути вгорі ДО того, як миготливий жовтий
    // дозволить рух, тому підняття замовляється тут, а червоний тримається,
    // поки воно не завершиться.
    if (s_night_wanted && s_phase == Phase::Red) {
        barrier_raise();
        if (s_barrier != Barrier::Up) {
            hold_phase("blinking yellow would allow movement under a moving boom");
            return;
        }
        ESP_LOGI(TAG, "mode: entering blinking yellow after red");
        enter_phase(Phase::YellowBlink);
        return;
    }

    const Phase next = def_of(s_phase).next;

    // Зелений не засвічується, поки стріла не вгорі. При нинішніх тривалостях
    // спрацювати не може - static_assert уже вимагає, щоб червоний+жовтий був
    // довший за хід, - але саме тут це правило записане виконуваним кодом, а
    // не лише перевіркою збірки.
    if (next == Phase::Green && s_barrier != Barrier::Up) {
        hold_phase("green must not come on before the boom is up");
        return;
    }

    if (next == Phase::Green) {
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
        ESP_LOGI(TAG, "mode: leaving blinking yellow through a steady yellow (edges: %" PRIu32 ")",
                 edges);
        enter_phase(def_of(Phase::YellowBlink).next);
        return;
    }
    ESP_LOGI(TAG, "mode: blinking yellow %s, applies after the red phase (edges: %" PRIu32 ")",
             s_night_wanted ? "requested" : "cancelled", edges);
}

// ─────────────────────────── входи ───────────────────────────

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
    if (now_pressed == pressed) return;
    pressed = now_pressed;
    if (!pressed) return;

    const Event ev = Event::ModeToggle;
    xQueueSend(s_events, &ev, 0);
}

// ─────────────────────────── ініціалізація ───────────────────────────

static void configure_gpio()
{
    gpio_config_t lamps = {};
    lamps.pin_bit_mask = (1ULL << Config::RED_PIN) | (1ULL << Config::YELLOW_PIN) |
                         (1ULL << Config::GREEN_PIN);
    lamps.mode      = GPIO_MODE_OUTPUT;
    lamps.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&lamps));
    apply_lamps(0);

    gpio_config_t button = {};
    button.pin_bit_mask = 1ULL << Config::BUTTON_PIN;
    button.mode         = GPIO_MODE_INPUT;
    button.pull_up_en   = GPIO_PULLUP_ENABLE;
    button.intr_type    = GPIO_INTR_ANYEDGE;
    ESP_ERROR_CHECK(gpio_config(&button));
}

static TickType_t wait_ticks(int64_t until_us, int64_t now_us)
{
    if (until_us == INT64_MAX) return portMAX_DELAY;
    const int64_t left_us = until_us - now_us;
    if (left_us <= 0) return 0;
    const TickType_t ticks = pdMS_TO_TICKS((left_us + 999) / 1000);
    return ticks ? ticks : 1;
}

extern "C" void app_main()
{
    s_events = xQueueCreate(Config::EVENT_QUEUE_LEN, sizeof(Event));
    ESP_ERROR_CHECK(s_events != nullptr ? ESP_OK : ESP_ERR_NO_MEM);

    const esp_timer_create_args_t settle_args = {
        .callback              = settle_timer_cb,
        .arg                   = nullptr,
        .dispatch_method       = ESP_TIMER_TASK,
        .name                  = "settle",
        .skip_unhandled_events = false,
    };
    ESP_ERROR_CHECK(esp_timer_create(&settle_args, &s_settle_timer));

    configure_gpio();
    servo_init();
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(Config::BUTTON_PIN, button_isr, nullptr));

    ESP_LOGI(TAG, "ready: PDR cycle %" PRIu32 "+%" PRIu32 "+%" PRIu32 "+%" PRIu32 "+%" PRIu32
                  " ms, boom budget %" PRIu32 " ms (travel %" PRIu32 " + ledc %" PRIu32 "), "
                  "servo GPIO %d at %" PRIu32 " Hz, boom position is modelled from time",
             Config::GREEN_MS, Config::GREEN_BLINK_MS, Config::YELLOW_MS, Config::RED_MS,
             Config::RED_YELLOW_MS, Config::BARRIER_BUDGET_MS, Config::BARRIER_TRAVEL_MS,
             Config::LEDC_MARGIN_MS, ServoCfg::PIN, ServoCfg::FREQ_HZ);

    // Стріла при старті в невідомому положенні - звідси окремий стан
    // Unknown: це не «внизу» і не «вгорі», це просто ще нічого не відомо.
    // Перше, що робить прошивка, - опускає стрілу під червоним. Піднімати
    // на старті не можна: це означало б відкрити проїзд, нічого про нього
    // не знаючи.
    enter_phase(Phase::Red);
    barrier_lower();

    while (true) {
        const int64_t   now = esp_timer_get_time();
        const PhaseDef &def = def_of(s_phase);

        const int64_t phase_end_us =
            def.duration_ms ? s_phase_started_us + def.duration_ms * 1000LL : INT64_MAX;
        const int64_t blink_at_us =
            def.blink_ms ? s_blink_toggled_us + def.blink_ms * 1000LL : INT64_MAX;

        // Стріла першою: її доїзд може розблокувати рішення автомата в цій же
        // ітерації.
        if (now >= s_barrier_deadline_us) {
            barrier_budget_expired();
            continue;
        }
        if (now >= phase_end_us) {
            advance_phase();
            continue;
        }
        if (now >= blink_at_us) {
            s_blink_on         = !s_blink_on;
            s_blink_toggled_us = blink_at_us;
            apply_lamps(s_blink_on ? def.lamps : 0);
            continue;
        }

        int64_t next_us = phase_end_us < blink_at_us ? phase_end_us : blink_at_us;
        if (s_barrier_deadline_us < next_us) next_us = s_barrier_deadline_us;

        Event ev;
        if (xQueueReceive(s_events, &ev, wait_ticks(next_us, now)) == pdTRUE) {
            on_mode_toggle();
        }
    }
}
