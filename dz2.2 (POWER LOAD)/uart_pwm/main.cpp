#include <Arduino.h>

// ДЗ 2.2, експеримент: UART як джерело ШІМ-сигналу.
//
// Кадр 8N1 — це 10 біт: старт (завжди 0), вісім біт даних молодшим уперед і
// стоп (завжди 1). Якщо гнати той самий байт без пауз, на лінії TX виходить
// періодичний сигнал із заповненням, яке задає сам байт:
//
//   0x00 -> 0 00000000 1  -> 1 біт з 10 високий  -> 10 %
//   0x0F -> 0 11110000 1  -> 5 біт з 10          -> 50 %, одним широким імпульсом
//   0x55 -> 0 10101010 1  -> 5 біт з 10          -> 50 %, причому разом зі стартом
//                                                    наступного кадру виходить чистий меандр baud/2
//   0xFF -> 0 11111111 1  -> 9 біт з 10          -> 90 %
//
// Частоту задає швидкість порту: один кадр = 10 біт, тобто f = baud / 10.
//
// TX_PIN   -> навантаження (світлодіод через резистор або база ключа) і CH0 аналізатора
// SENSE_PIN <- перемичка з TX_PIN, щоб прошивка сама зміряла реальне заповнення

struct Config {
  static constexpr uint8_t TX_PIN    = 17;  // Serial1 TX
  static constexpr uint8_t RX_PIN    = 18;  // не використовується, але Serial1 хоче пін
  static constexpr uint8_t SENSE_PIN = 7;   // перемичка з TX_PIN

  static constexpr uint32_t MEASURE_WINDOW_US  = 50000;  // вікно виміру заповнення
  static constexpr uint32_t REPORT_INTERVAL_MS = 1000;
  static constexpr uint32_t AUTO_INTERVAL_MS   = 4000;   // як довго тримати кожен байт

  static constexpr uint16_t CHUNK       = 128;   // байтів за одне звертання до Serial1
  static constexpr uint16_t TX_BUFFER   = 2048;  // кільцевий буфер передачі Serial1
  static constexpr uint32_t SERIAL_BAUD = 115200;
};

struct Pattern {
  uint8_t     byte;
  const char* note;
};

// Порядок у таблиці — це і порядок клавіш 1..4 у моніторі.
static constexpr Pattern PATTERNS[] = {
  { 0x00, "усі біти даних нулі, високий тільки стоп" },
  { 0x0F, "чотири одиниці підряд — широкий імпульс і вузький стоп" },
  { 0x55, "біти чергуються — на лінії чистий меандр baud/2" },
  { 0xFF, "усі біти даних одиниці, низький тільки старт" },
};
static constexpr uint8_t PATTERN_COUNT = sizeof(PATTERNS) / sizeof(PATTERNS[0]);

static constexpr uint32_t BAUDS[] = { 2400, 9600, 19200, 57600, 115200, 230400 };
static constexpr uint8_t BAUD_COUNT = sizeof(BAUDS) / sizeof(BAUDS[0]);

static_assert(PATTERN_COUNT <= 9, "клавіші 1..9 — більше патернів не влізе");

// Скільки біт кадру високі: стоп-біт плюс одиниці в даних. Старт завжди низький.
static constexpr uint8_t highBitsInFrame(uint8_t value) {
  return 1 + __builtin_popcount(value);
}

static uint8_t patternIndex = 0;
static uint8_t baudIndex    = 1;  // 9600 — кадр 1.04 мс, видно і оком, і аналізатором
static bool    streaming    = true;

// Прошивка сама перебирає байти по колу, щоб експеримент працював і без введення:
// монітор, запущений як задача IDE, показує вивід, але не передає натискання в порт.
// Будь-яка натиснута клавіша вимикає автоперебір і віддає керування рукам.
static bool autoCycle = true;

static uint8_t chunk[Config::CHUNK];

static_assert(Config::TX_BUFFER >= Config::CHUNK, "буфер має вміщати хоча б один шматок");

static uint32_t baud() { return BAUDS[baudIndex]; }
static uint8_t  patternByte() { return PATTERNS[patternIndex].byte; }

static void fillChunk() {
  for (uint16_t i = 0; i < Config::CHUNK; ++i) chunk[i] = patternByte();
}

// Доливаємо в кільцевий буфер передачі рівно один шматок за виклик. Драйвер UART
// далі віддає байти в лінію сам, у перериванні, тому потік не рветься, навіть коли
// loop() зайнятий чимось іншим.
//
// Саме один, а не «поки є місце»: Serial1.write() блокує, доки байти не влізуть
// у буфер, і цикл while тут здатен не вийти взагалі. loop() крутиться тисячі
// разів на секунду, тож буфер і так не спорожніє.
static void topUpStream() {
  if (!streaming) return;
  if (Serial1.availableForWrite() >= Config::CHUNK) {
    Serial1.write(chunk, Config::CHUNK);
  }
}

static void restartStream() {
  Serial1.end();
  Serial1.setTxBufferSize(Config::TX_BUFFER);  // тільки до begin(), інакше драйвер уже піднятий
  Serial1.begin(baud(), SERIAL_8N1, Config::RX_PIN, Config::TX_PIN);
  fillChunk();
}

// Заповнення міряємо статистично: у вікні MEASURE_WINDOW_US просто рахуємо,
// скільки разів пін прочитався високим. Переривання тут не годяться — на
// 115200 бод фронт приходить кожні 8.7 мкс, і ISR з'їла б увесь процесор.
static uint8_t measureDutyPercent() {
  topUpStream();  // на час виміру loop() стоїть, тому буфер має бути повним заздалегідь

  uint32_t high = 0;
  uint32_t total = 0;
  const uint32_t startUs = micros();

  while (micros() - startUs < Config::MEASURE_WINDOW_US) {
    if (digitalRead(Config::SENSE_PIN) == HIGH) ++high;
    ++total;
  }

  return total == 0 ? 0 : static_cast<uint8_t>(high * 100 / total);
}

static void printState() {
  const uint8_t  value    = patternByte();
  const uint8_t  highBits = highBitsInFrame(value);
  const uint32_t frameUs  = 10 * 1000000UL / baud();
  const uint32_t highUs   = highBits * 1000000UL / baud();

  Serial.println();
  Serial.printf("байт 0x%02X (%s)\r\n", value, PATTERNS[patternIndex].note);
  Serial.printf("кадр: 0 %c%c%c%c%c%c%c%c 1  (молодший біт першим)\r\n",
                (value >> 0) & 1 ? '1' : '0', (value >> 1) & 1 ? '1' : '0',
                (value >> 2) & 1 ? '1' : '0', (value >> 3) & 1 ? '1' : '0',
                (value >> 4) & 1 ? '1' : '0', (value >> 5) & 1 ? '1' : '0',
                (value >> 6) & 1 ? '1' : '0', (value >> 7) & 1 ? '1' : '0');
  Serial.printf("швидкість %u бод: біт %u ns, кадр %u us (%u Hz)\r\n",
                baud(), 1000000000UL / baud(), frameUs, baud() / 10);
  Serial.printf("теоретично: %u біт з 10 високі -> %u %%, високий рівень %u us на кадр\r\n",
                highBits, highBits * 10, highUs);
  Serial.printf("потік: %s\r\n", streaming ? "увімкнено" : "зупинено (лінія в idle, тобто HIGH)");
}

static void printHelp() {
  Serial.println();
  Serial.println("клавіші в моніторі:");
  for (uint8_t i = 0; i < PATTERN_COUNT; ++i) {
    Serial.printf("  %u  - байт 0x%02X, заповнення %u %%\r\n",
                  i + 1, PATTERNS[i].byte, highBitsInFrame(PATTERNS[i].byte) * 10);
  }
  Serial.println("  +/- - наступна / попередня швидкість порту");
  Serial.println("  s   - пауза або продовження потоку");
  Serial.println("  ?   - ця підказка");
  Serial.println();
  Serial.printf("поки клавіш не було, байти перебираються самі, по %u ms на кожен\r\n",
                Config::AUTO_INTERVAL_MS);
}

static void handleKey(char key) {
  if (autoCycle && key != '?') {
    autoCycle = false;
    Serial.println();
    Serial.println("автоперебір вимкнено, керування з клавіш");
  }

  if (key >= '1' && key < '1' + PATTERN_COUNT) {
    patternIndex = key - '1';
    fillChunk();
    printState();
    return;
  }

  switch (key) {
    case '+':
      if (baudIndex + 1 < BAUD_COUNT) ++baudIndex;
      restartStream();
      printState();
      break;

    case '-':
      if (baudIndex > 0) --baudIndex;
      restartStream();
      printState();
      break;

    case 's':
      streaming = !streaming;
      printState();
      break;

    case '?':
      printHelp();
      break;

    default:
      break;
  }
}

void setup() {
  Serial.begin(Config::SERIAL_BAUD);

  pinMode(Config::SENSE_PIN, INPUT_PULLUP);
  restartStream();

  delay(300);

  Serial.println();
  Serial.println("=== DZ 2.2: UART as a PWM source ===");
  Serial.printf("TX на GPIO %u, перемичка на GPIO %u для виміру заповнення\r\n",
                Config::TX_PIN, Config::SENSE_PIN);
  printHelp();
  printState();
  Serial.println();
  Serial.println(" байт | бод    | кадр us | теорія % | виміряно % | вільно в буфері");
  Serial.println("------+--------+---------+----------+------------+----------------");
}

void loop() {
  static uint32_t lastReportMs = 0;
  static uint32_t lastSwitchMs = 0;

  // 1. Тримаємо буфер передачі повним — інакше між кадрами з'являться паузи,
  // лінія піде в idle (HIGH), і заповнення поповзе вгору.
  topUpStream();

  // 2. Клавіші з монітора.
  while (Serial.available()) {
    handleKey(static_cast<char>(Serial.read()));
  }

  const uint32_t now = millis();

  // 3. Автоперебір байтів, поки ніхто не втручався.
  if (autoCycle && now - lastSwitchMs >= Config::AUTO_INTERVAL_MS) {
    lastSwitchMs = now;
    patternIndex = (patternIndex + 1) % PATTERN_COUNT;
    fillChunk();
    printState();
  }

  // 4. Звіт із реально зміряним заповненням.
  if (now - lastReportMs >= Config::REPORT_INTERVAL_MS) {
    lastReportMs = now;

    const uint8_t measured = measureDutyPercent();
    Serial.printf(" 0x%02X | %6u | %7u | %7u%% | %9u%% | %15d\r\n",
                  patternByte(), baud(), 10 * 1000000UL / baud(),
                  highBitsInFrame(patternByte()) * 10, measured,
                  Serial1.availableForWrite());
  }
}
