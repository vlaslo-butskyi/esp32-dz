# ДЗ 6.5 — спільний код (common)

Ця папка містить код ДЗ 6.5, який не залежить від плати. Обидві прошивки, [`conveyor`](../conveyor/) для ESP32-S3 і [`conveyor_stm32`](../conveyor_stm32/) для STM32F411, збирають ці самі файли. Код не викликає FreeRTOS і драйвери, тому тести перевіряють його на ПК.

| Файл | Що містить |
|---|---|
| [`press_filter.h`](press_filter.h), [`press_filter.c`](press_filter.c) | фільтр брязкоту для ISR датчика деталі, той самий, що в ДЗ 6.3 |
| [`rate.h`](rate.h), [`rate.c`](rate.c) | швидкість конвеєра за останні 10 с, деталі/хв × 10 |
| [`report.h`](report.h), [`report.c`](report.c) | рядки звіту і дописування хвоста з перевіркою місця |
| [`test/test_conveyor.c`](test/test_conveyor.c), [`test/run.sh`](test/run.sh) | тести на ПК |
| [`CMakeLists.txt`](CMakeLists.txt) | компонент ESP-IDF для нативної збірки |

Прошивка плати містить ISR, задачі, спінлок або критичну секцію, м'ютекс і семафори. Буфер звіту захищає прошивка. Функції `report_*` тільки форматують рядки в буфер, який їм передали.

| Що робить прошивка плати | Яку функцію викликає |
|---|---|
| на кожному фронті датчика в ISR | `press_filter_edge(&filter, released, now_ms)`: `true` - нова деталь |
| `report` раз на секунду | `rate_update(&rate, parts)`, потім `report_begin()` у локальний рядок |
| `adc` після кожного звіту | `report_pot()` у локальний рядок, потім `report_append()` під м'ютексом |
| `estop` | записує `REPORT_HALTED` під м'ютексом |

`report_append()` не обрізає звіт. Якщо хвіст не вміщується, функція не змінює буфер і повертає `false`. Тоді `adc` виводить `report buffer full`.

## Тести на ПК

Запустіть з кореня репо:

```sh
"dz6.5 (MUTEX SEMAPHORE)/common/test/run.sh"
```

Скрипт збирає тести компілятором ПК з `-std=c11 -Wall -Wextra -Wpedantic -Werror` і запускає їх. Кожен тест викликає функції напряму і сам задає час і значення лічильника.

| Тест | Що перевіряє |
|---|---|
| `part: clean press` | деталь, відпускання, друга деталь |
| `part: bounce` | брязкіт при натисканні і при відпусканні дає одну деталь, хоча ISR прочитав 0 при відпусканні |
| `rate: first seconds` | у перші секунди вікно коротше: 2 деталі за 1 с = 120,0 деталей/хв |
| `rate: 10 s window` | одна деталь щосекунди = 60,0/хв; після зупинки конвеєра 30,0/хв через 5 с і 0 через 10 с |
| `rate: counter wraparound` | лічильник перейшов через 0xFFFFFFFF, швидкість правильна |
| `report: text` | `parts 20, rate 114.5/min; pot 3134 mV` |
| `report: buffer full` | хвіст, який не вміщується разом з `'\0'`, не змінює звіт |

Результат: `40 checks, 0 failed`.

Тести перевірено помилками. Кожну помилку внесли в код окремо, і тести її знайшли.

| Помилка в коді | Тест, який упав |
|---|---|
| ISR не чекає тиші перед деталлю | `part: bounce` |
| ISR визначає деталь за рівнем піна в момент переривання | `part: bounce` |
| швидкість завжди ділиться на 10 с, також у перші секунди | `rate: first seconds` |
| вікно швидкості росте без межі | `rate: 10 s window` |
| `report_append()` не лишає місця для `'\0'` | `report: buffer full` |

## Збірка

| Збірка | Як додає `common` |
|---|---|
| ESP-IDF, `tools/idf.sh` | `EXTRA_COMPONENT_DIRS` у [`../conveyor/CMakeLists.txt`](../conveyor/CMakeLists.txt), компонент [`CMakeLists.txt`](CMakeLists.txt) |
| PlatformIO, env `idf_conveyor` | [`src/CMakeLists.txt`](../../src/CMakeLists.txt) додає `*.c` з папки `common` поруч з варіантом |
| PlatformIO, env `stm32_conveyor` | `build_src_filter` і `build_flags -I` у [`platformio.ini`](../../platformio.ini) |
