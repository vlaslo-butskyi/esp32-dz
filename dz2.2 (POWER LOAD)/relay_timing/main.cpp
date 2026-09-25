#include <Arduino.h>

// ДЗ 2.2, завдання 1: скільки часу минає від команди на GPIO до того, як
// механіка реле реально замкне (розімкне) сухий контакт.
//
// COIL_PIN    -> R 10 кОм -> база BC547B -> колектор на вхід IN модуля реле
// CONTACT_PIN <- контакт NO реле, COM реле посаджений на GND
//
// Контакт читається з INPUT_PULLUP: розімкнутий контакт = HIGH (тягне підтяжка),
// замкнутий = LOW (COM притискає пін до землі).

struct Config {
  // Периферія
  // GPIO 8 і GPIO 2 — звичайні піни. GPIO 3 тут не годиться: це strapping-пін
  // ESP32-S3, той самий, через який були проблеми в ДЗ 1.7.
  static constexpr uint8_t COIL_PIN    = 8;  // керування обмоткою (через ключ на модуль реле)
  static constexpr uint8_t CONTACT_PIN = 2;  // сухий контакт NO, INPUT_PULLUP

  // Полярності. RELAY_ON_LEVEL підбирається під модуль: ключ на BC547B інвертує
  // сигнал, тому з оптомодулем «low level trigger» реле вмикається рівнем HIGH.
  static constexpr uint8_t RELAY_ON_LEVEL       = HIGH;
  static constexpr uint8_t CONTACT_CLOSED_LEVEL = LOW;

  // Час
  static constexpr uint32_t SETTLE_US  = 5000;    // 5 мс без фронтів = брязкіт стих
  static constexpr uint32_t TIMEOUT_US = 100000;  // 100 мс мовчання = реле не відповіло
  static constexpr uint32_t HOLD_MS    = 300;     // пауза між командами

  // Серія
  static constexpr uint16_t CYCLES = 15;  // стільки разів увімкнути й вимкнути

  static constexpr uint32_t SERIAL_BAUD = 115200;
};

static_assert(Config::CYCLES >= 10, "завдання вимагає щонайменше 10 вимірювань");
static_assert(Config::TIMEOUT_US > Config::SETTLE_US, "таймаут має бути довшим за вікно заспокоєння");

constexpr uint8_t inverted(uint8_t level) {
  return level == HIGH ? LOW : HIGH;
}

// --- захоплення фронтів у перериванні ---------------------------------------
//
// ISR не вирішує, який фронт «справжній», а тільки запам'ятовує час першого,
// час останнього і скільки їх було. Перший фронт — це момент, коли контакт
// вперше торкнувся; останній — коли він перестав брязкотіти. Різниця між ними
// і є механічний брязкіт, через який наївне «перше переривання = готово»
// дає купу зайвих спрацювань.

static portMUX_TYPE edgeMux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t firstEdgeUs = 0;
static volatile uint32_t lastEdgeUs  = 0;
static volatile uint16_t edgeCount   = 0;

void IRAM_ATTR onContactEdge() {
  const uint32_t now = micros();

  portENTER_CRITICAL_ISR(&edgeMux);
  if (edgeCount == 0) firstEdgeUs = now;
  lastEdgeUs = now;
  ++edgeCount;
  portEXIT_CRITICAL_ISR(&edgeMux);
}

struct Capture {
  uint32_t first;
  uint32_t last;
  uint16_t count;
};

static Capture snapshot() {
  Capture c;
  portENTER_CRITICAL(&edgeMux);
  c.first = firstEdgeUs;
  c.last  = lastEdgeUs;
  c.count = edgeCount;
  portEXIT_CRITICAL(&edgeMux);
  return c;
}

static void resetCapture() {
  portENTER_CRITICAL(&edgeMux);
  firstEdgeUs = 0;
  lastEdgeUs  = 0;
  edgeCount   = 0;
  portEXIT_CRITICAL(&edgeMux);
}

// --- статистика --------------------------------------------------------------

class Stats {
  private:
    uint32_t sum_ = 0;
    uint32_t min_ = UINT32_MAX;
    uint32_t max_ = 0;
    uint16_t count_ = 0;

  public:
    void add(uint32_t value) {
      sum_ += value;
      if (value < min_) min_ = value;
      if (value > max_) max_ = value;
      ++count_;
    }

    uint32_t average() const { return count_ == 0 ? 0 : sum_ / count_; }
    uint32_t minimum() const { return count_ == 0 ? 0 : min_; }
    uint32_t maximum() const { return max_; }
    uint16_t count() const { return count_; }
};

// --- одне перемикання --------------------------------------------------------

struct Measurement {
  uint32_t firstUs  = 0;  // від команди до першого фронту — власне спрацювання
  uint32_t settleUs = 0;  // від команди до останнього фронту — кінець брязкоту
  uint16_t edges    = 0;
  bool     ok       = false;
};

static Measurement switchRelay(bool on) {
  const uint8_t coilLevel = on ? Config::RELAY_ON_LEVEL : inverted(Config::RELAY_ON_LEVEL);
  const uint8_t expected  = on ? Config::CONTACT_CLOSED_LEVEL : inverted(Config::CONTACT_CLOSED_LEVEL);

  resetCapture();

  // Точка відліку і сама команда стоять поруч: усе, що між ними, лягло б у вимір.
  const uint32_t commandUs = micros();
  digitalWrite(Config::COIL_PIN, coilLevel);

  Measurement m;

  while (true) {
    const Capture c = snapshot();
    const uint32_t now = micros();

    if (c.count > 0) {
      m.firstUs  = c.first - commandUs;
      m.settleUs = c.last - commandUs;
      m.edges    = c.count;
    }

    // Контакт вважається таким, що зайняв нове положення, коли він протримався
    // без жодного фронту довше за SETTLE_US.
    if (c.count > 0 && now - c.last >= Config::SETTLE_US) {
      m.ok = digitalRead(Config::CONTACT_PIN) == expected;
      break;
    }

    if (now - commandUs >= Config::TIMEOUT_US) {
      m.ok = false;
      break;
    }
  }

  return m;
}

// --- перевірка ланцюга -------------------------------------------------------
//
// Переривання тут навмисно ні до чого: обидва рівні подаються з паузою і контакт
// просто читається. Якщо ця перевірка не проходить, міряти мікросекунди нема сенсу —
// спочатку треба полагодити схему або полярність.

static void selfTest() {
  Serial.println("--- перевірка ланцюга, без переривань ---");

  digitalWrite(Config::COIL_PIN, Config::RELAY_ON_LEVEL);
  delay(50);
  const int whenOn = digitalRead(Config::CONTACT_PIN);

  digitalWrite(Config::COIL_PIN, inverted(Config::RELAY_ON_LEVEL));
  delay(50);
  const int whenOff = digitalRead(Config::CONTACT_PIN);

  Serial.printf("GPIO %u = %s -> контакт %s\r\n",
                Config::COIL_PIN, Config::RELAY_ON_LEVEL == HIGH ? "HIGH" : "LOW",
                whenOn == Config::CONTACT_CLOSED_LEVEL ? "замкнутий" : "розімкнутий");
  Serial.printf("GPIO %u = %s -> контакт %s\r\n",
                Config::COIL_PIN, Config::RELAY_ON_LEVEL == HIGH ? "LOW" : "HIGH",
                whenOff == Config::CONTACT_CLOSED_LEVEL ? "замкнутий" : "розімкнутий");

  if (whenOn == whenOff) {
    Serial.println("ПРОБЛЕМА: контакт не рухається взагалі.");
    Serial.println("  перевір ланцюг GPIO -> R -> база -> колектор -> IN реле,");
    Serial.println("  живлення модуля (VCC/GND) і те, що COM сидить на землі.");
  } else if (whenOn != Config::CONTACT_CLOSED_LEVEL) {
    Serial.println("ПРОБЛЕМА: контакт рухається, але у зворотний бік.");
    Serial.println("  постав RELAY_ON_LEVEL на протилежний рівень і перезбери.");
  } else {
    Serial.println("ланцюг у порядку, міряю час");
  }
  Serial.println();
}

// --- серія вимірювань --------------------------------------------------------

static void printSummaryLine(const char* label, const Stats& s) {
  Serial.printf("%-26s avg %6u us (%u.%02u ms) | min %6u us | max %6u us\r\n",
                label,
                s.average(), s.average() / 1000, (s.average() % 1000) / 10,
                s.minimum(), s.maximum());
}

static void runSeries() {
  Stats onFirst, onBounce, offFirst, offBounce;
  uint32_t onEdgesTotal = 0, offEdgesTotal = 0;
  uint16_t failures = 0;

  Serial.println();
  Serial.println("   # | on: t1 | on: ts | on: bnc | n | off: t1 | off: ts | off: bnc | n");
  Serial.println("-----+--------+--------+---------+---+---------+---------+----------+---");

  for (uint16_t i = 1; i <= Config::CYCLES; ++i) {
    const Measurement on = switchRelay(true);
    delay(Config::HOLD_MS);

    const Measurement off = switchRelay(false);
    delay(Config::HOLD_MS);

    Serial.printf("%4u | %6u | %6u | %7u | %u | %7u | %7u | %8u | %u%s\r\n",
                  i,
                  on.firstUs, on.settleUs, on.settleUs - on.firstUs, on.edges,
                  off.firstUs, off.settleUs, off.settleUs - off.firstUs, off.edges,
                  (on.ok && off.ok) ? "" : "   <- промах");

    if (on.ok) {
      onFirst.add(on.firstUs);
      onBounce.add(on.settleUs - on.firstUs);
      onEdgesTotal += on.edges;
    } else {
      ++failures;
    }

    if (off.ok) {
      offFirst.add(off.firstUs);
      offBounce.add(off.settleUs - off.firstUs);
      offEdgesTotal += off.edges;
    } else {
      ++failures;
    }
  }

  Serial.println();
  Serial.printf("=== підсумок за %u циклів ===\r\n", Config::CYCLES);
  printSummaryLine("увімкнення (pull-in)", onFirst);
  printSummaryLine("брязкіт при увімкненні", onBounce);
  printSummaryLine("вимкнення (release)", offFirst);
  printSummaryLine("брязкіт при вимкненні", offBounce);

  if (onFirst.count() > 0) {
    Serial.printf("фронтів при увімкненні: %u на %u вимірів (%u.%u на спрацювання)\r\n",
                  onEdgesTotal, onFirst.count(),
                  onEdgesTotal / onFirst.count(), (onEdgesTotal * 10 / onFirst.count()) % 10);
  }
  if (offFirst.count() > 0) {
    Serial.printf("фронтів при вимкненні:  %u на %u вимірів (%u.%u на спрацювання)\r\n",
                  offEdgesTotal, offFirst.count(),
                  offEdgesTotal / offFirst.count(), (offEdgesTotal * 10 / offFirst.count()) % 10);
  }
  if (failures > 0) {
    Serial.printf("промахів: %u (контакт не зайняв очікуване положення за %u us)\r\n",
                  failures, Config::TIMEOUT_US);
  }

  Serial.println();
  Serial.println("надішли будь-який символ у монітор, щоб повторити серію");
}

void setup() {
  Serial.begin(Config::SERIAL_BAUD);

  pinMode(Config::COIL_PIN, OUTPUT);
  digitalWrite(Config::COIL_PIN, inverted(Config::RELAY_ON_LEVEL));

  pinMode(Config::CONTACT_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(Config::CONTACT_PIN), onContactEdge, CHANGE);

  delay(500);  // дати реле відпустити, а підтяжці — встановитись

  Serial.println();
  Serial.println("=== DZ 2.2: relay switching time ===");
  Serial.printf("обмотка на GPIO %u (реле вмикається рівнем %s)\r\n",
                Config::COIL_PIN, Config::RELAY_ON_LEVEL == HIGH ? "HIGH" : "LOW");
  Serial.printf("контакт на GPIO %u (INPUT_PULLUP, переривання по CHANGE, замкнутий = %s)\r\n",
                Config::CONTACT_PIN, Config::CONTACT_CLOSED_LEVEL == LOW ? "LOW" : "HIGH");
  Serial.printf("вікно заспокоєння %u us, таймаут %u us, пауза між командами %u ms\r\n",
                Config::SETTLE_US, Config::TIMEOUT_US, Config::HOLD_MS);
  Serial.printf("стартовий стан контакту: %s\r\n",
                digitalRead(Config::CONTACT_PIN) == Config::CONTACT_CLOSED_LEVEL ? "замкнутий" : "розімкнутий");
  Serial.println();
  Serial.println("t1  - від команди до першого фронту (спрацювання механіки)");
  Serial.println("ts  - від команди до останнього фронту (брязкіт стих)");
  Serial.println("bnc - тривалість брязкоту, ts - t1;  n - скільки фронтів побачив МК");
}

void loop() {
  static bool done = false;

  if (!done) {
    selfTest();
    runSeries();
    done = true;
    return;
  }

  if (Serial.available()) {
    while (Serial.available()) Serial.read();
    done = false;
  }

  delay(50);
}
