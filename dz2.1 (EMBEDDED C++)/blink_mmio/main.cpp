#include <Arduino.h>

struct BlinkConfig {
  static constexpr uint8_t  LED_PIN           = 16;
  static constexpr uint32_t BLINK_INTERVAL_MS = 500;
  static constexpr uint32_t SERIAL_BAUD       = 115200;
  static constexpr uint16_t BENCH_ITERATIONS  = 1000;
};

// Карта регістрів GPIO для ESP32-S3. Адреси звірені з заголовками SDK:
// framework-arduinoespressif32/tools/sdk/esp32s3/include/soc/esp32s3/include/soc/
//   soc.h (DR_REG_GPIO_BASE, DR_REG_IO_MUX_BASE), gpio_reg.h, io_mux_reg.h
// У бойовому коді ці заголовки підключають, а не переписують — тут вони виписані
// вручну навмисне, щоб уся адресація була видимою і constexpr.
namespace Reg {
  constexpr uint32_t GPIO_BASE   = 0x60004000;
  constexpr uint32_t OUT_W1TS    = GPIO_BASE + 0x008; // запис одиниці -> пін у HIGH
  constexpr uint32_t OUT_W1TC    = GPIO_BASE + 0x00C; // запис одиниці -> пін у LOW
  constexpr uint32_t ENABLE_W1TS = GPIO_BASE + 0x024; // запис одиниці -> увімкнути драйвер виходу

  // Матриця GPIO: який сигнал іде на пін. 256 = «просто GPIO_OUT», а не периферія.
  constexpr uint32_t SIG_GPIO_OUT = 256;
  constexpr uint32_t funcOutSel(uint8_t pin) { return GPIO_BASE + 0x554 + 4u * pin; }

  // IO_MUX: на S3 регістри йдуть підряд, по 4 байти на пін, починаючи з 0x04.
  constexpr uint32_t IO_MUX_BASE   = 0x60009000;
  constexpr uint32_t MCU_SEL_SHIFT = 12;
  constexpr uint32_t MCU_SEL_MASK  = 0x7u << MCU_SEL_SHIFT;
  constexpr uint32_t FUNC_GPIO     = 1; // функція 1 — звичайний GPIO
  constexpr uint32_t ioMux(uint8_t pin) { return IO_MUX_BASE + 0x04 + 4u * pin; }

  // Єдине місце, де адреса стає вказівником. volatile тут обов'язковий: без нього
  // компілятор вважає запис у цю адресу звичайною роботою з пам'яттю і має право
  // його викинути, переставити або злити кілька записів в один.
  inline volatile uint32_t* at(uint32_t address) {
    return reinterpret_cast<volatile uint32_t*>(address);
  }
} // namespace Reg

enum class LedState : uint8_t {
  Off = 0,
  On  = 1,
};

class Led {
  private:
    const uint8_t pin_;
    const uint32_t mask_;
    LedState state_ = LedState::Off;

  public:
    constexpr explicit Led(uint8_t pin) : pin_(pin), mask_(1u << pin) {}

    // Ініціалізація периферії на рівні регістрів, три кроки і жодної магії:
    void init() {
      // 1. IO_MUX: вивести пін з альтернативної функції у звичайний GPIO.
      volatile uint32_t* const ioMux = Reg::at(Reg::ioMux(pin_));
      *ioMux = (*ioMux & ~Reg::MCU_SEL_MASK) | (Reg::FUNC_GPIO << Reg::MCU_SEL_SHIFT);

      // 2. Матриця GPIO: на вихід подати вміст GPIO_OUT, а не сигнал периферії.
      *Reg::at(Reg::funcOutSel(pin_)) = Reg::SIG_GPIO_OUT;

      // 3. Увімкнути драйвер виходу для цього піна.
      *Reg::at(Reg::ENABLE_W1TS) = mask_;

      set(LedState::Off);
    }

    // Один запис у регістр, без читання. W1TS/W1TC влаштовані так, що одиниця
    // в біті виставляє або скидає свій пін, а нулі не чіпають сусідні — тому тут
    // не потрібен цикл read-modify-write, який міг би побитися з ISR.
    void set(LedState state) {
      *Reg::at(state == LedState::On ? Reg::OUT_W1TS : Reg::OUT_W1TC) = mask_;
      state_ = state;
    }

    void toggle() {
      set(state_ == LedState::On ? LedState::Off : LedState::On);
    }

    LedState state() const { return state_; }
};

static Led led(BlinkConfig::LED_PIN);

// Скільки коштує смикнути пін через Arduino і скільки — через регістр.
// Обидві функції роблять по два записи за ітерацію.
static uint32_t benchDigitalWrite(uint8_t pin, uint16_t iterations) {
  const uint32_t start = micros();
  for (uint16_t i = 0; i < iterations; ++i) {
    digitalWrite(pin, HIGH);
    digitalWrite(pin, LOW);
  }
  return micros() - start;
}

static uint32_t benchRegister(uint32_t mask, uint16_t iterations) {
  volatile uint32_t* const outSet   = Reg::at(Reg::OUT_W1TS);
  volatile uint32_t* const outClear = Reg::at(Reg::OUT_W1TC);

  const uint32_t start = micros();
  for (uint16_t i = 0; i < iterations; ++i) {
    *outSet = mask;
    *outClear = mask;
  }
  return micros() - start;
}

// Дві однакові функції, які відрізняються рівно одним словом. Ніхто їх не
// викликає — вони існують, щоб подивитися в дизасемблер і побачити, що робить
// volatile. Розбір у README.
void storeVolatile(uint32_t mask) {
  volatile uint32_t* out = Reg::at(Reg::OUT_W1TS);
  for (uint8_t i = 0; i < 8; ++i) {
    *out = mask;
  }
}

void storePlain(uint32_t mask) {
  uint32_t* out = reinterpret_cast<uint32_t*>(Reg::OUT_W1TS);
  for (uint8_t i = 0; i < 8; ++i) {
    *out = mask;
  }
}

void setup() {
  Serial.begin(BlinkConfig::SERIAL_BAUD);

  led.init();

  const uint32_t viaArduino = benchDigitalWrite(BlinkConfig::LED_PIN, BlinkConfig::BENCH_ITERATIONS);
  const uint32_t viaRegister = benchRegister(1u << BlinkConfig::LED_PIN, BlinkConfig::BENCH_ITERATIONS);
  const uint32_t writes = 2u * BlinkConfig::BENCH_ITERATIONS;

  Serial.println();
  Serial.println("=== DZ 2.1: blink via GPIO registers (MMIO) ===");
  Serial.printf(
    "LED on GPIO %u, mask 0x%08X, interval %u ms\r\n",
    BlinkConfig::LED_PIN, 1u << BlinkConfig::LED_PIN, BlinkConfig::BLINK_INTERVAL_MS
  );
  Serial.printf(
    "IO_MUX 0x%08X, GPIO_FUNC%u_OUT_SEL 0x%08X, OUT_W1TS 0x%08X, OUT_W1TC 0x%08X\r\n",
    Reg::ioMux(BlinkConfig::LED_PIN), BlinkConfig::LED_PIN,
    Reg::funcOutSel(BlinkConfig::LED_PIN), Reg::OUT_W1TS, Reg::OUT_W1TC
  );
  Serial.println();
  Serial.printf("%u writes each:\r\n", writes);
  Serial.printf(
    "  digitalWrite()   %6u us total, %4u ns per write\r\n",
    viaArduino, viaArduino * 1000u / writes
  );
  Serial.printf(
    "  GPIO_OUT_W1TS/C  %6u us total, %4u ns per write\r\n",
    viaRegister, viaRegister * 1000u / writes
  );
  Serial.printf("  speedup x%u\r\n", viaRegister == 0 ? 0 : viaArduino / viaRegister);
  Serial.println();
}

void loop() {
  static uint32_t lastToggleMs = 0;

  const uint32_t now = millis();

  if (now - lastToggleMs < BlinkConfig::BLINK_INTERVAL_MS) return;

  lastToggleMs = now;
  led.toggle();

  Serial.printf(
    "[%8u ms] LED %s\r\n",
    now, led.state() == LedState::On ? "ON" : "OFF"
  );
}
