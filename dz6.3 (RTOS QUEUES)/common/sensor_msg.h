// Повідомлення черги і таблиця джерел для ДЗ 6.3.
// Спільний код: без FreeRTOS, без драйверів, збирається для ESP32-S3, STM32F411 і ПК.
#ifndef DZ63_SENSOR_MSG_H
#define DZ63_SENSOR_MSG_H

#include <stdint.h>

enum {
  TEMP_PERIOD_MS  = 1000,
  POT_PERIOD_MS   = 250,
  SEND_TIMEOUT_MS = 10,     // продюсер чекає місця в черзі
  RECV_TIMEOUT_MS = 500,    // consumer чекає повідомлення
  SILENCE_PERIODS = 3,      // стільки періодів без даних = датчик зник
  STATS_PERIOD_MS = 10000,
  QUEUE_LENGTH    = 8,
  BUTTON_QUIET_MS = 30,     // тиша на піні перед натисканням
};

_Static_assert(SEND_TIMEOUT_MS < POT_PERIOD_MS, "send timeout must fit in a period");
_Static_assert(RECV_TIMEOUT_MS < SILENCE_PERIODS * POT_PERIOD_MS,
               "consumer must check silence more often than the shortest window");

typedef enum {
  SOURCE_TEMP,
  SOURCE_POT,
  SOURCE_BUTTON,
  SOURCE_COUNT,
} source_t;

// Черга копіює всі 16 байт, тому відправник одразу може перевикористати змінну.
// source - uint8_t, а не source_t: enum займає 4 байти.
typedef struct {
  uint8_t  source;   // source_t: від кого
  uint8_t  core;     // ядро, на якому працював відправник; на STM32 завжди 0
  uint32_t seq;      // номер від цього джерела: розрив = втрачені повідомлення
  uint32_t time_ms;  // момент вимірювання або натискання
  int32_t  value;    // temp: соті частки °C; pot: мВ; button: 0
} sensor_msg_t;

_Static_assert(sizeof(sensor_msg_t) == 16, "message is copied into the queue, keep it small");

typedef struct {
  const char *name;       // і ім'я задачі продюсера
  uint32_t    period_ms;  // 0 - джерело без періоду (кнопка)
} source_info_t;

// Індекс у таблиці = номер джерела.
static const source_info_t SOURCES[SOURCE_COUNT] = {
  [SOURCE_TEMP]   = {"temp",   TEMP_PERIOD_MS},
  [SOURCE_POT]    = {"pot",    POT_PERIOD_MS},
  [SOURCE_BUTTON] = {"button", 0},
};

#endif
