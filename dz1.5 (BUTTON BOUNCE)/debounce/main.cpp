#include <Arduino.h>

constexpr uint8_t BUTTON_IN = 16;     // Кнопка підключена до GPIO 16, друга ніжка → GND

// Новий стан кнопки приймаємо, лише коли після останнього фронту минуло DEBOUNCE_MS.
// Постав сюди значення з висновку ДЗ (../README.md), а потім зменшуй,
// поки не з'являться зайві PRESS: так знайдеш мінімальний робочий debounce.
constexpr uint8_t DEBOUNCE_MS = 10;

volatile uint32_t edge_counter = 0;     // Усі фронти (FALLING і RISING), які побачив МК
volatile uint32_t last_edge_us = 0;

portMUX_TYPE edge_mux = portMUX_INITIALIZER_UNLOCKED;

bool stable_low = false;          // Відфільтрований стан: true = натиснута
uint32_t press_counter = 0;
uint32_t edges_at_last_change = 0;
uint32_t stable_since_us = 0;

void IRAM_ATTR button_isr() {
  uint32_t now = micros();

  portENTER_CRITICAL_ISR(&edge_mux);
  edge_counter++;
  last_edge_us = now;
  portEXIT_CRITICAL_ISR(&edge_mux);
}

void setup() {
  pinMode(BUTTON_IN, INPUT_PULLUP);
  Serial.begin(115200);
  delay(500);

  stable_low = digitalRead(BUTTON_IN) == LOW;
  stable_since_us = micros();

  Serial.println();
  Serial.println("=== DZ 1.5: debounced button ===");
  Serial.printf("GPIO %d, INPUT_PULLUP, interrupt on CHANGE, debounce %d ms\n", BUTTON_IN, DEBOUNCE_MS);

  // CHANGE: потрібні обидва фронти, щоб знати, коли брязкіт повністю затих
  attachInterrupt(digitalPinToInterrupt(BUTTON_IN), button_isr, CHANGE);
}

void loop() {
  portENTER_CRITICAL(&edge_mux);
  uint32_t edges = edge_counter;
  uint32_t last_us = last_edge_us;
  portEXIT_CRITICAL(&edge_mux);

  bool is_low = digitalRead(BUTTON_IN) == LOW;
  uint32_t now = micros();

  if (is_low != stable_low && now - last_us >= DEBOUNCE_MS * 1000UL) {
    uint32_t transition_edges = edges - edges_at_last_change;
    uint32_t held_us = now - stable_since_us;

    stable_low = is_low;
    stable_since_us = now;
    edges_at_last_change = edges;

    if (stable_low) {
      press_counter++;
      Serial.printf("PRESS   #%-3u edges: %-3u ignored: %-3u (released for %lu ms)\n",
                    press_counter, transition_edges, transition_edges > 0 ? transition_edges - 1 : 0,
                    held_us / 1000);
    } else {
      Serial.printf("RELEASE #%-3u edges: %-3u ignored: %-3u (held for %lu ms)\n",
                    press_counter, transition_edges, transition_edges > 0 ? transition_edges - 1 : 0,
                    held_us / 1000);
    }
  }

  delay(1);
}
