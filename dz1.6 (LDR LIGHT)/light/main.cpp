#include <Arduino.h>

// Подільник як в умові: 3V3 → LDR → середня точка → 10 кОм → GND. Середня точка на GPIO 1 (ADC1_CH0)
constexpr uint8_t LDR_AIN = 1;

constexpr uint8_t SAMPLE_PERIOD_MS = 100;   // Період вимірювання з умови ДЗ
constexpr double ADC_MAX = 4095.0;           // ADCmax для 12-бітного АЦП
constexpr double UREF_MV = 3100.0;           // Uref з умови, мВ (3.1 В)
constexpr uint8_t HEADER_EVERY = 20;         // Через скільки рядків підбивати підсумок і повторювати шапку

uint32_t last_sample_ms = 0;
uint32_t sample_number = 0;

// Статистика похибки в межах одного блоку таблиці
uint32_t block_count = 0;
double block_err_sum = 0.0;
double block_err_abs_max = 0.0;

// У printf() переклад рядка треба писати руками: Serial.println() додає \r\n, а printf() — лише те,
// що в форматі. З голим \n монітор порту опускає курсор, але не повертає його на початок рядка.
void print_header() {
  Serial.println("    # |   RAW | U(calc), mV | U(calc), V | U(API), mV |  dU, mV |  err, %");
  Serial.println("------+-------+-------------+------------+------------+---------+--------");
}

void print_block_summary() {
  Serial.printf("      avg err: %+.2f %%   max |err|: %.2f %%   (%u samples)\r\n\r\n",
                block_err_sum / block_count, block_err_abs_max, block_count);

  block_count = 0;
  block_err_sum = 0.0;
  block_err_abs_max = 0.0;
}

void setup() {
  Serial.begin(115200);
  delay(500);

  analogReadResolution(12);                     // 0..4095, тобто ADCmax = 4095 як в умові
  analogSetPinAttenuation(LDR_AIN, ADC_11db);   // Найбільше послаблення: вхідний діапазон ~0..3.1 В

  Serial.println();
  Serial.println("=== DZ 1.6: LDR light meter ===");
  Serial.printf("GPIO %d (ADC1), period %d ms, ADCmax %.0f, Uref %.0f mV\r\n",
                LDR_AIN, SAMPLE_PERIOD_MS, ADC_MAX, UREF_MV);
  Serial.println("U(calc) = RAW * Uref / ADCmax,  U(API) = analogReadMilliVolts()");
  Serial.println("dU = U(calc) - U(API),  err = dU / U(API) * 100%");
  Serial.println();

  print_header();
  last_sample_ms = millis();
}

void loop() {
  // Рівно один вимір на SAMPLE_PERIOD_MS; якщо loop() десь затримався, лічильник наздожене сам
  if (millis() - last_sample_ms < SAMPLE_PERIOD_MS) {
    return;
  }
  last_sample_ms += SAMPLE_PERIOD_MS;

  // Два окремі перетворення: спершу сирий код, одразу за ним — те саме через калібровану функцію
  int raw = analogRead(LDR_AIN);
  uint32_t u_api_mv = analogReadMilliVolts(LDR_AIN);

  double u_calc_mv = raw * UREF_MV / ADC_MAX;   // Формула з умови
  double diff_mv = u_calc_mv - (double)u_api_mv;
  double err_pct = u_api_mv > 0 ? diff_mv / (double)u_api_mv * 100.0 : 0.0;

  sample_number++;
  Serial.printf("%5u | %5d | %11.1f | %10.3f | %10u | %7.1f | %7.2f\r\n",
                sample_number, raw, u_calc_mv, u_calc_mv / 1000.0, u_api_mv, diff_mv, err_pct);

  block_count++;
  block_err_sum += err_pct;
  if (fabs(err_pct) > block_err_abs_max) {
    block_err_abs_max = fabs(err_pct);
  }

  if (block_count == HEADER_EVERY) {
    print_block_summary();
    print_header();
  }
}
