#include <Arduino.h>
#include <esp_system.h>

// ДЗ 2.2, завдання 2: керування швидкістю навантаження програмним ШІМ.
// Положення потенціометра -> ADC -> тривалість високого й низького рівня на GPIO.
//
// POT_PIN   <- середня точка потенціометра (крайні виводи на 3V3 і GND)
// MOTOR_PIN -> R 220 Ом -> база 2N2222A -> колектор на мінус двигуна
//              плюс двигуна на пін 5V плати, паралельно двигуну flyback-діод 1N4007,
//              по живленню електролітичний конденсатор 100...470 мкФ
//
// Окремого джерела для двигуна в мене немає, тому силова частина висить на тій
// самій шині 5V, що й плата. Через це кидок струму при старті просаджує живлення
// мікроконтролера, і конденсатор перестає бути порадою — без нього плата ловить
// brownout. Причина ребуту друкується в setup(), щоб це було видно в логу.
//
// 220 Ом у базі дають струм керування (3.3 - 1.1) / 220 = 10 мА: пін ESP32 не
// перевантажений, а 2N2222A при такому струмі бази входить у насичення аж до
// 400 мА колекторного струму — для TT-моторчика з запасом.
//
// Апаратний LEDC тут навмисно не використовується: увесь сенс завдання в тому,
// щоб побачити, скільки коштує формувати ШІМ руками в superloop.

struct Config {
  // Периферія
  static constexpr uint8_t POT_PIN   = 1;  // ADC1_CH0
  static constexpr uint8_t MOTOR_PIN = 2;  // база ключа

  // ШІМ
  static constexpr uint32_t PWM_PERIOD_US = 1000;  // 1 кГц
  // Обидва числа зміряні на моєму двигуні: зі спокою він зривається на 50 %,
  // а вже розкручений тримає оберти аж до 25 %. Різниця — тертя спокою.
  static constexpr uint8_t  MIN_DUTY      = 25;    // менше двигун уже не тягне
  static constexpr uint8_t  MAX_DUTY      = 100;
  // Поштовх розімкнутий: датчика обертів немає, тому прошивка не знає, чи двигун
  // уже рушив. Лишається підібрати рівень трохи вище порога зриву і тривалість
  // настільки коротку, щоб розгін не встиг перескочити задані оберти.
  static constexpr uint8_t  KICK_DUTY     = 58;    // трохи вище зміряних 50 %
  static constexpr uint32_t KICK_MS       = 80;
  static constexpr uint8_t  RAMP_STEP     = 2;     // максимальна зміна заповнення за один
                                                   // вимір ADC: 0 -> 100 % розтягується на секунду

  // АЦП
  static constexpr uint16_t ADC_MAX          = 4095;
  static constexpr uint16_t ADC_DEADBAND     = 60;  // мертва зона біля нуля, щоб «вимкнено» було вимкнено
  static constexpr uint8_t  ADC_WINDOW       = 8;   // ковзне середнє
  static constexpr uint32_t ADC_INTERVAL_MS  = 20;  // один вимір за раз, щоб не гальмувати ШІМ

  // Діагностика
  static constexpr uint32_t REPORT_INTERVAL_MS = 500;
  static constexpr uint32_t SERIAL_BAUD        = 115200;
};

static_assert(Config::PWM_PERIOD_US >= 100, "занадто короткий період для програмного ШІМ");
static_assert(Config::MIN_DUTY < Config::MAX_DUTY, "MIN_DUTY має бути меншим за MAX_DUTY");
static_assert(Config::RAMP_STEP > 0, "без кроку розгону заповнення ніколи не дожене ручку");
static_assert(Config::KICK_DUTY > Config::MIN_DUTY, "поштовх слабший за робочий мінімум не має сенсу");
static_assert(Config::ADC_WINDOW > 0 && (Config::ADC_WINDOW & (Config::ADC_WINDOW - 1)) == 0,
              "розмір вікна має бути степенем двійки — ділення перетвориться на зсув");

// --- програмний ШІМ ----------------------------------------------------------
//
// Кожен виклик tick() тільки дивиться на micros() і, якщо час настав, перекидає
// пін. Нічого не чекає і нічого не блокує, тому loop() лишається вільним.
// Заодно рахує, наскільки фронт спізнився проти запланованого часу — це і є
// ціна програмного ШІМ порівняно з апаратним LEDC.

class SoftPwm {
  private:
    const uint8_t  pin_;
    const uint32_t periodUs_;

    uint32_t highUs_ = 0;
    uint32_t lowUs_  = Config::PWM_PERIOD_US;

    uint32_t nextEdgeUs_ = 0;
    bool     level_      = false;

    uint32_t lateSumUs_ = 0;
    uint32_t lateMaxUs_ = 0;
    uint32_t edges_     = 0;

    void write(bool level) {
      if (level == level_) return;
      level_ = level;
      digitalWrite(pin_, level_ ? HIGH : LOW);
    }

  public:
    constexpr SoftPwm(uint8_t pin, uint32_t periodUs)
      : pin_(pin), periodUs_(periodUs) {}

    void init() {
      pinMode(pin_, OUTPUT);
      digitalWrite(pin_, LOW);
      level_ = false;
      nextEdgeUs_ = micros();
    }

    // Головне перетворення завдання: відсоток -> тривалість високого й низького рівня.
    void setDuty(uint8_t percent) {
      if (percent > 100) percent = 100;
      highUs_ = periodUs_ * percent / 100;
      lowUs_  = periodUs_ - highUs_;
    }

    void tick() {
      const uint32_t now = micros();

      // Крайні положення — це не ШІМ, а просто рівень. Фронтів не робимо взагалі.
      if (highUs_ == 0) { write(false); nextEdgeUs_ = now; return; }
      if (lowUs_ == 0)  { write(true);  nextEdgeUs_ = now; return; }

      // Порівняння через різницю зі знаком коректно переживає переповнення micros().
      if (static_cast<int32_t>(now - nextEdgeUs_) < 0) return;

      const uint32_t lateUs = now - nextEdgeUs_;
      lateSumUs_ += lateUs;
      if (lateUs > lateMaxUs_) lateMaxUs_ = lateUs;
      ++edges_;

      write(!level_);

      // Наступний фронт рахуємо від запланованого часу, а не від now —
      // інакше період повзе вперед на величину кожного запізнення.
      nextEdgeUs_ += level_ ? highUs_ : lowUs_;

      // Якщо loop() проспав більше за цілий інтервал, наздоганяти вже нічого:
      // просто починаємо відлік від поточного моменту.
      if (static_cast<int32_t>(now - nextEdgeUs_) > 0) {
        nextEdgeUs_ = now + (level_ ? highUs_ : lowUs_);
      }
    }

    uint32_t highUs() const { return highUs_; }
    uint32_t lowUs() const { return lowUs_; }
    uint32_t edges() const { return edges_; }
    uint32_t lateAverageUs() const { return edges_ == 0 ? 0 : lateSumUs_ / edges_; }
    uint32_t lateMaxUs() const { return lateMaxUs_; }

    void resetStats() {
      lateSumUs_ = 0;
      lateMaxUs_ = 0;
      edges_     = 0;
    }
};

// --- потенціометр ------------------------------------------------------------
//
// Один вимір за виклик і ковзне середнє по вікну: analogRead() на ESP32 шумить
// на десятки одиниць, а читати 8 разів поспіль не можна — це витягнуло б
// ітерацію настільки, що ШІМ поїхав би.

class Potentiometer {
  private:
    const uint8_t pin_;
    uint16_t window_[Config::ADC_WINDOW] = {0};
    uint8_t  index_ = 0;
    uint32_t sum_ = 0;

  public:
    constexpr explicit Potentiometer(uint8_t pin) : pin_(pin) {}

    void init() {
      analogReadResolution(12);
      analogSetPinAttenuation(pin_, ADC_11db);  // вхід до ~3.1 В

      const uint16_t first = analogRead(pin_);
      for (uint8_t i = 0; i < Config::ADC_WINDOW; ++i) window_[i] = first;
      sum_ = static_cast<uint32_t>(first) * Config::ADC_WINDOW;
    }

    void sample() {
      const uint16_t raw = analogRead(pin_);
      sum_ -= window_[index_];
      window_[index_] = raw;
      sum_ += raw;
      index_ = (index_ + 1) % Config::ADC_WINDOW;
    }

    uint16_t value() const { return sum_ / Config::ADC_WINDOW; }
};

// ADC -> відсоток заповнення. Нижня мертва зона робить «вимкнено» справді
// вимкненим, а решта ходу розтягується від MIN_DUTY до MAX_DUTY: двигун
// рушає одразу, як тільки ручка зійшла з нуля, а не після третини обороту.
static uint8_t dutyFromAdc(uint16_t adc) {
  if (adc <= Config::ADC_DEADBAND) return 0;

  const uint32_t span  = Config::ADC_MAX - Config::ADC_DEADBAND;
  const uint32_t range = Config::MAX_DUTY - Config::MIN_DUTY;
  const uint32_t duty  = Config::MIN_DUTY + (adc - Config::ADC_DEADBAND) * range / span;

  return duty > Config::MAX_DUTY ? Config::MAX_DUTY : static_cast<uint8_t>(duty);
}

// Двигун живиться з тієї ж шини 5V, що й плата, тому просадка живлення при старті
// мотора може скинути мікроконтролер. ESP_RST_BROWNOUT у логу — це саме той випадок,
// і лікується він конденсатором по живленню, а не кодом.
static const char* resetReasonName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:  return "POWERON (подали живлення)";
    case ESP_RST_SW:       return "SW (програмний перезапуск)";
    case ESP_RST_PANIC:    return "PANIC (виключення в коді)";
    case ESP_RST_BROWNOUT: return "BROWNOUT (просіло живлення!)";
    case ESP_RST_TASK_WDT:
    case ESP_RST_INT_WDT:
    case ESP_RST_WDT:      return "WDT (сторожовий таймер)";
    case ESP_RST_EXT:      return "EXT (кнопка RST)";
    default:               return "інша";
  }
}

static SoftPwm pwm(Config::MOTOR_PIN, Config::PWM_PERIOD_US);
static Potentiometer pot(Config::POT_PIN);

void setup() {
  Serial.begin(Config::SERIAL_BAUD);

  pwm.init();
  pot.init();

  delay(300);

  Serial.println();
  Serial.println("=== DZ 2.2: software PWM from a potentiometer ===");
  Serial.printf("причина останнього старту: %s\r\n", resetReasonName(esp_reset_reason()));
  Serial.printf("потенціометр на GPIO %u (ADC1, 12 біт), ключ на GPIO %u\r\n",
                Config::POT_PIN, Config::MOTOR_PIN);
  Serial.printf("період %u us (%u Hz), робочий діапазон %u..%u %%, мертва зона ADC < %u\r\n",
                Config::PWM_PERIOD_US, 1000000UL / Config::PWM_PERIOD_US,
                Config::MIN_DUTY, Config::MAX_DUTY, Config::ADC_DEADBAND);
  Serial.printf("ADC: одне читання за %u ms, ковзне середнє по %u вимірах\r\n",
                Config::ADC_INTERVAL_MS, Config::ADC_WINDOW);
  Serial.printf("розгін: не більше %u %% за вимір, тобто 0 -> 100 %% за %u ms\r\n",
                Config::RAMP_STEP, 100 / Config::RAMP_STEP * Config::ADC_INTERVAL_MS);
  Serial.printf("поштовх при старті: %u %% протягом %u ms\r\n",
                Config::KICK_DUTY, Config::KICK_MS);
  Serial.println();
  Serial.println("   ADC | ціль | зараз | high us | low us | фронт/с | ШІМ late | max | ADC avg | max | print");
  Serial.println("-------+------+-------+---------+--------+---------+----------+------+---------+-----+------");
}

// Плавний розгін: заповнення не стрибає за ручкою, а доганяє її кроками. Різкий
// перехід з нуля на повну дав би кидок струму, який просаджує ту саму шину 5V,
// з якої живиться плата. Заразом це рятує редуктор від ударного старту.
static uint8_t rampToward(uint8_t current, uint8_t target) {
  if (target > current) {
    return (target - current > Config::RAMP_STEP) ? current + Config::RAMP_STEP : target;
  }
  if (target < current) {
    return (current - target > Config::RAMP_STEP) ? current - Config::RAMP_STEP : target;
  }
  return current;
}

void loop() {
  static uint32_t lastAdcMs    = 0;
  static uint32_t lastReportMs = 0;
  static uint8_t  duty         = 0;
  static uint8_t  targetDuty   = 0;

  // Скільки коштує сам analogRead(). Підозра в тому, що саме він затримує фронт
  // ШІМ, тому міряємо його окремо, а не списуємо на «щось у системі».
  static uint32_t adcSumUs   = 0;
  static uint32_t adcMaxUs   = 0;
  static uint16_t adcSamples = 0;

  static uint32_t kickUntilMs = 0;
  static bool     kicked      = false;

  // Скільки коштував друк попереднього рядка. Свій власний друк виміряти й
  // надрукувати в тому ж рядку не вийде, тому колонка показує минуле вікно.
  static uint32_t printUs = 0;

  // 1. ШІМ — найперше в ітерації і без жодних умов перед ним.
  pwm.tick();

  const uint32_t now = millis();

  // 2. Потенціометр: один вимір раз на ADC_INTERVAL_MS.
  if (now - lastAdcMs >= Config::ADC_INTERVAL_MS) {
    lastAdcMs = now;

    const uint32_t adcStartUs = micros();
    pot.sample();
    const uint32_t adcUs = micros() - adcStartUs;

    adcSumUs += adcUs;
    if (adcUs > adcMaxUs) adcMaxUs = adcUs;
    ++adcSamples;

    const uint8_t requested = dutyFromAdc(pot.value());

    // Рушання зі спокою коштує дорожче за підтримання обертів: двигун зривається
    // з місця на 50 %, а крутиться далі й на 25 %. Тому при переході з нуля даємо
    // короткий поштовх на KICK_DUTY, а далі віддаємо керування ручці — інакше вся
    // нижня половина її ходу була б мертвою.
    if (requested > 0 && targetDuty == 0) {
      kickUntilMs = now + Config::KICK_MS;
      kicked = true;
    }
    if (requested == 0) {
      kickUntilMs = 0;
    }
    targetDuty = requested;

    if (kickUntilMs != 0) {
      if (static_cast<int32_t>(now - kickUntilMs) < 0) {
        // Поштовх свідомо йде повз плавний розгін: у цьому вся його суть.
        duty = Config::KICK_DUTY;
      } else {
        // Поштовх скінчився — падаємо на задане одразу, без плавного сповзання.
        // Обмежувати швидкість зниження нема сенсу: кидок струму буває на
        // розгоні, а не тоді, коли заповнення зменшується. Якщо сповзати
        // розгоном, двигун ще третину секунди крутиться швидше за задане.
        kickUntilMs = 0;
        duty = targetDuty;
      }
    } else {
      duty = rampToward(duty, targetDuty);
    }

    pwm.setDuty(duty);
  }

  // 3. Звіт. Друк дорогий, тому рівно раз на REPORT_INTERVAL_MS.
  if (now - lastReportMs >= Config::REPORT_INTERVAL_MS) {
    const uint32_t windowMs = now - lastReportMs;
    lastReportMs = now;

    const uint32_t printStartUs = micros();
    Serial.printf("%6u | %3u%% | %4u%% | %7u | %6u | %7u | %8u | %4u | %7u | %4u | %6u%s\r\n",
                  pot.value(), targetDuty, duty, pwm.highUs(), pwm.lowUs(),
                  windowMs == 0 ? 0 : pwm.edges() * 1000 / windowMs,
                  pwm.lateAverageUs(), pwm.lateMaxUs(),
                  adcSamples == 0 ? 0 : adcSumUs / adcSamples, adcMaxUs,
                  printUs,
                  kicked ? "  <- поштовх" : "");
    printUs = micros() - printStartUs;

    kicked = false;
    pwm.resetStats();
    adcSumUs   = 0;
    adcMaxUs   = 0;
    adcSamples = 0;
  }
}
