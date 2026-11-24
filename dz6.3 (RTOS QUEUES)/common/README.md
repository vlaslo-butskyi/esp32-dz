# ДЗ 6.3 — спільний код (common)

Ця папка містить код ДЗ 6.3, який не залежить від плати. Обидві прошивки, [`sensor_queue`](../sensor_queue/) для ESP32-S3 і [`sensor_queue_stm32`](../sensor_queue_stm32/) для STM32F411, збирають ці самі файли. Код не викликає FreeRTOS, драйвери і `printf`, тому тести перевіряють його на ПК.

| Файл | Що містить |
|---|---|
| [`sensor_msg.h`](sensor_msg.h) | повідомлення черги `sensor_msg_t`, таблиця джерел `SOURCES[]`, періоди й таймаути |
| [`press_filter.h`](press_filter.h), [`press_filter.c`](press_filter.c) | фільтр натискань для ISR кнопки |
| [`consumer.h`](consumer.h), [`consumer.c`](consumer.c) | облік у `consumer`: розриви `seq`, тиша датчиків, статистика, рядки логу |
| [`test/test_queue.c`](test/test_queue.c), [`test/run.sh`](test/run.sh) | тести на ПК |
| [`CMakeLists.txt`](CMakeLists.txt) | компонент ESP-IDF для нативної збірки |

Прошивка плати читає датчики, створює задачі й чергу, обробляє переривання і виводить лог.

| Що робить прошивка плати | Яку функцію викликає |
|---|---|
| на кожному фронті кнопки в ISR | `press_filter_edge(&filter, released, now_ms)`: `true` - натискання |
| після кожного `xQueueReceive` з повідомленням | `consumer_note_depth()`, потім `consumer_handle()` |
| після кожного `xQueueReceive`, також після таймауту | `consumer_tick()`: тиша датчиків і статистика |
| виводить рядок | функція `log`, яку прошивка передає в `consumer_init()` |

Функція `log` отримує рядок без часу. ESP32-версія додає час і викликає `printf`. STM32-версія додає час і передає рядок у USART1 під м'ютексом.

## Тести на ПК

Запустіть з кореня репо:

```sh
"dz6.3 (RTOS QUEUES)/common/test/run.sh"
```

Скрипт збирає тести компілятором ПК з `-std=c11 -Wall -Wextra -Wpedantic -Werror` і запускає їх. Кожен тест викликає функції напряму і сам задає час у мілісекундах. Тести `consumer` збирають рядки логу в масив і шукають потрібний рядок.

| Тест | Що перевіряє |
|---|---|
| `button: clean press` | натискання, відпускання, друге натискання |
| `button: bounce on press` | 5 фронтів за 3 мс дають одне натискання |
| `button: bounce on release` | брязкіт при відпусканні не дає натискання, хоча ISR прочитав 0 |
| `consumer: values` | рядки `temp` з від'ємним значенням, `pot` і `button`, поле `wait` |
| `consumer: lost messages` | від `pot` прийшли `#0` і `#3`: `lost 2 before #3` |
| `consumer: unknown source` | номер джерела поза таблицею: повідомлення відкинуто |
| `consumer: silence` | `no data` через 3 періоди, один раз; `back online`; повторна тиша |
| `consumer: stats` | статистика рівно через 10 с, глибина черги не більше 8 |

Результат: `30 checks, 0 failed`.

Тести перевірено помилками. Кожну помилку внесли в код окремо, і тести її знайшли.

| Помилка в коді | Тест, який упав |
|---|---|
| ISR визначає натискання за рівнем піна в момент переривання | `button: bounce on release` |
| ISR не чекає тиші перед натисканням | `button: bounce on press` |
| розрив у `seq` не рахується | `consumer: lost messages` |
| `back online` не скидає прапорець тиші | `consumer: silence` |
| глибина черги без обмеження зверху | `consumer: stats` |

Першу помилку з цієї таблиці мала перша версія прошивки. Тоді її знайшов прогін у Wokwi, тепер її ловить тест на ПК.

## Збірка

| Збірка | Як додає `common` |
|---|---|
| ESP-IDF, `tools/idf.sh` | `EXTRA_COMPONENT_DIRS` у [`../sensor_queue/CMakeLists.txt`](../sensor_queue/CMakeLists.txt), компонент [`CMakeLists.txt`](CMakeLists.txt) |
| PlatformIO, env `idf_sensor_queue` | [`src/CMakeLists.txt`](../../src/CMakeLists.txt) додає `*.c` з папки `common` поруч з варіантом |
| PlatformIO, env `stm32_sensor_queue` | `build_src_filter` і `build_flags -I` у [`platformio.ini`](../../platformio.ini) |
