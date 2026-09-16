#include <Arduino.h>

#define BUTTON_IN 16           // Кнопка підключена до GPIO 16, друга ніжка → GND

#define BURST_GAP_MS 30        // Тиша довша за це = брязкіт закінчився, наступний фронт відкриває нову пачку
#define MAX_EDGES 64           // Скільки часових міток фронтів зберігати в одній пачці
#define PRINT_EDGE_TIMES 1     // 1 = друкувати час кожного FALLING-фронту відносно першого (мкс)

volatile uint32_t button_counter = 0;  // Усі FALLING-переривання від старту (як у базовому коді)

// Поточна пачка брязкоту. ISR і loop() звертаються до неї під спільним spinlock,
// бо це кілька змінних, і loop() має прочитати їх узгоджено.
volatile uint32_t burst_counter = 0;
volatile unsigned long burst_last_us = 0;
unsigned long burst_edges_us[MAX_EDGES];

portMUX_TYPE burst_mux = portMUX_INITIALIZER_UNLOCKED;

uint32_t press_number = 0;
bool pressed = false;             // Чи вважаємо кнопку зараз натиснутою
unsigned long high_since_ms = 0;  // Коли loop() востаннє побачив HIGH без нових переривань (для "чистого" відпускання)

// Швидка функція обробки переривань (ISR), яка розміщується в оперативній пам'яті (IRAM)
void IRAM_ATTR button_isr() {
  unsigned long now = micros();

  portENTER_CRITICAL_ISR(&burst_mux);
  if (burst_counter < MAX_EDGES) {
    burst_edges_us[burst_counter] = now;
  }
  burst_counter++;
  burst_last_us = now;
  button_counter++;
  portEXIT_CRITICAL_ISR(&burst_mux);
}

void print_burst(const char* kind, uint32_t count, const unsigned long* edges_us, uint32_t total) {
  uint32_t stored = min(count, (uint32_t)MAX_EDGES);
  unsigned long span_us = edges_us[stored - 1] - edges_us[0];

  Serial.printf("%-7s #%-2u FALLING: %-3u span: %8.3f ms  total: %u\n",
                kind, press_number, count, span_us / 1000.0, total);

#if PRINT_EDGE_TIMES
  if (count > 1) {
    Serial.print("         edges, us:");
    for (uint32_t i = 0; i < stored; i++) {
      Serial.printf(" %lu", edges_us[i] - edges_us[0]);
    }
    if (count > stored) {
      Serial.printf(" ... (+%u not stored)", count - stored);
    }
    Serial.println();
  }
#endif
}

void setup() {
  pinMode(BUTTON_IN, INPUT_PULLUP);
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("=== DZ 1.5: button bounce counter ===");
  Serial.printf("GPIO %d, INPUT_PULLUP, interrupt on FALLING, burst gap %d ms\n", BUTTON_IN, BURST_GAP_MS);
  Serial.println("PRESS/HOLD = pin LOW after burst, RELEASE = pin HIGH after burst, TAP = press+release inside one burst");

  // Прив'язка переривання. FALLING означає спрацьовування при натисканні (перехід з 1 в 0)
  attachInterrupt(digitalPinToInterrupt(BUTTON_IN), button_isr, FALLING);
}

void loop() {
  uint32_t count = 0;
  uint32_t total = 0;
  unsigned long edges_us[MAX_EDGES];

  // Пачка закінчилась, якщо після останнього FALLING-фронту минуло BURST_GAP_MS
  portENTER_CRITICAL(&burst_mux);
  if (burst_counter > 0 && micros() - burst_last_us > BURST_GAP_MS * 1000UL) {
    count = burst_counter;
    total = button_counter;
    memcpy(edges_us, burst_edges_us, sizeof(unsigned long) * min(count, (uint32_t)MAX_EDGES));
    burst_counter = 0;
  }
  bool burst_pending = burst_counter > 0;
  portEXIT_CRITICAL(&burst_mux);

  bool is_low = digitalRead(BUTTON_IN) == LOW;

  if (count > 0) {
    if (is_low && pressed) {
      // Контакти "затріщали" під час утримання (натиснули сильніше): це не нове натискання
      print_burst("HOLD", count, edges_us, total);
    } else if (is_low) {
      press_number++;
      pressed = true;
      print_burst("PRESS", count, edges_us, total);
    } else if (pressed) {
      // FALLING-фронти під час відпускання: базовий код порахував би їх як зайві натискання
      pressed = false;
      print_burst("RELEASE", count, edges_us, total);
    } else {
      // Натиснули й відпустили швидше, ніж BURST_GAP_MS: усе злилося в одну пачку
      press_number++;
      print_burst("TAP", count, edges_us, total);
    }
    high_since_ms = millis();
  } else if (pressed && !is_low && !burst_pending) {
    // Відпускання без жодного FALLING-фронту: чекаємо BURST_GAP_MS стабільного HIGH
    if (millis() - high_since_ms > BURST_GAP_MS) {
      pressed = false;
      Serial.printf("RELEASE #%-2u FALLING: 0   (clean)\n", press_number);
    }
  } else {
    high_since_ms = millis();
  }

  delay(1);
}
