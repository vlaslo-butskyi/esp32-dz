# ДЗ 6.2 — four_tasks (RTOS SCHEDULER)

Прошивка для **ESP32-S3-DevKitC-1**. Чотири задачі FreeRTOS з різними пріоритетами пишуть рядки в UART. Схема не потрібна: прошивці досить плати й кабелю USB.

| Задача з умови | Ім'я в коді | Пріоритет | Значення | Що робить |
|---|---|---|---|---|
| Sensor Reader | `sensor` | Normal | 2 | кожні 500 мс виводить `[Sensor] Data read...` |
| Event Trigger | `event` | Low | 1 | кожні 3 с створює задачу `worker` |
| Worker Task | `worker` | High | 3 | виводить `[Worker] Processing event...` і видаляє себе |
| Maintenance Mode | `maintenance` | Realtime | 4 | кожні 10 с зупиняє `sensor` на 2 с |

## Як працює код

Файл: [`main/main.cpp`](main/main.cpp)

`app_main()` створює задачі `sensor`, `event` і `maintenance`. Задачу `worker` створює `event` під час роботи. Кожен рядок логу починається з номера тіку, тобто з часу в мілісекундах.

Усі задачі працюють на ядрі 1. На ESP32-S3 кожне ядро має свій планувальник. Задачі на різних ядрах працюють одночасно, і пріоритет між ними не діє.

### Sensor Reader

Задача рахує період через `vTaskDelayUntil`. Ця функція рахує наступне пробудження від попереднього пробудження. Час на `printf` не зсуває період.

```cpp
vTaskDelayUntil(&last_wake, period);

TickType_t late = xTaskGetTickCount() - last_wake;
last_wake += late - late % period;
```

Два рядки після `vTaskDelayUntil` потрібні через паузу від `maintenance`. Протягом паузи минають чотири моменти за розкладом. Після паузи `vTaskDelayUntil` повертається одразу за кожен пропущений момент. Ці рядки переносять `last_wake` на останній момент за розкладом. Перевірка в симуляторі без цих рядків показала такий лог:

```
  10005  [Maintenance] System paused for 2 seconds.
  12008  [Sensor] Data read...
  12009  [Sensor] Data read...
  12011  [Sensor] Data read...
  12013  [Sensor] Data read...
  12015  [Sensor] Data read...
  12505  [Sensor] Data read...
```

### Event Trigger і Worker Task

`event` чекає 3 с через `vTaskDelay`. Потім задача створює `worker` з пріоритетом High. Пріоритет `worker` вищий, тому планувальник запускає `worker` ще до повернення з `xTaskCreatePinnedToCore`.

`worker` виводить один рядок і викликає `vTaskDelete(nullptr)`. Стек і TCB задачі звільняє idle-задача ядра 1. Функція `worker_task` не доходить до `}`.

Якщо купі не вистачить пам'яті, `event` виведе `[Event] no memory for Worker Task`. Тоді `event` спробує знову через 3 с.

### Maintenance Mode

`maintenance` має найвищий пріоритет і прокидається через `vTaskDelayUntil` кожні 10 с:

```cpp
vTaskSuspend(sensor);
printf(... "[Maintenance] System paused for %lu seconds.\n" ...);
vTaskDelay(pdMS_TO_TICKS(Config::MAINTENANCE_PAUSE_MS));
vTaskResume(sensor);
```

`app_main()` передає `maintenance` handle задачі `sensor` як параметр. Якщо handle дорівнює `NULL`, `vTaskSuspend(NULL)` зупинить саму `maintenance`. Тому `start()` перериває прошивку через `abort()`, якщо задачу не вдалося створити.

Протягом паузи `event` і `worker` працюють далі. Пауза стосується тільки задачі `sensor`.

### Параметри

| Параметр | Значення | Причина |
|---|---|---|
| ядро | 1 | один планувальник на всі задачі |
| стек | 3072 байти | симулятор показав, що задача з `printf` використовує близько 1,1 КБ |
| тік | 1 мс | `CONFIG_FREERTOS_HZ=1000` у [`sdkconfig.defaults`](sdkconfig.defaults) |
| пріоритети | 1, 2, 3, 4 | вищі за idle-задачу (0) і нижчі за системні задачі ESP-IDF |

## Лог плати

Плата виконала прошивку 36 с після перезапуску. Прошивку зібрав PlatformIO, ESP-IDF 6.1.0. Фрагмент логу:

```
   9019  [Sensor] Data read...
   9019  [Worker] Processing event...
   9519  [Sensor] Data read...
  10019  [Maintenance] System paused for 2 seconds.
  12019  [Sensor] Data read...
  12019  [Worker] Processing event...
  12519  [Sensor] Data read...
  ...
  19519  [Sensor] Data read...
  20019  [Maintenance] System paused for 2 seconds.
  21019  [Worker] Processing event...
  22019  [Sensor] Data read...
  22519  [Sensor] Data read...
```

Повний лог: [`serial.log`](serial.log).

| Задача | Моменти в логу | Інтервал |
|---|---|---|
| `sensor` | 519, 1019 ... 9519, потім 12019 ... 19519 | 500 мс |
| `maintenance` | 10019, 20019, 30019 | 10 000 мс |
| `sensor` після паузи | 12019, 22019, 32019 | 2000 мс від рядка `maintenance` |
| `worker` | 3019, 6019 ... 33019 | 3000 мс |

Лог показує такі факти:
- Протягом паузи `sensor` не виводить рядків, а `worker` виводить. Приклад: рядок 21019.
- Після паузи `sensor` виводить один рядок і повертається до розкладу 519, 1019 і так далі.
- Якщо кілька задач готові в одному тіку, рядки йдуть за пріоритетом. У тіку 3019 першим виводить `sensor`, бо `event` має пріоритет Low. У тіку 30019 першим виводить `maintenance`.
- Рядок `worker` не зсувається, хоча `event` чекає через `vTaskDelay`. `worker` і `printf` займають менше за 1 мс, тому `vTaskDelay` починає відлік у тому самому тіку.

Симулятор Wokwi показав інший результат: рядок `worker` зсувався на 1 мс за цикл. У симуляторі `printf` повертається пізніше, ніж на платі. Лог симулятора: [`wokwi.log`](wokwi.log).

## Запуск

### На платі, нативний ESP-IDF 5.5.5

1. Підключіть плату через USB.
2. Запустіть з кореня репо:
   ```sh
   ./tools/idf.sh "dz6.2 (RTOS SCHEDULER)/four_tasks" flash monitor
   ```

### На платі, PlatformIO

1. Підключіть плату через USB.
2. Прошийте плату:
   ```sh
   pio run -e idf_four_tasks -t upload
   ```
3. Відкрийте монітор порту:
   ```sh
   pio device monitor -e idf_four_tasks
   ```

### У симуляторі (Wokwi for VS Code)

1. Зберіть прошивку: `pio run -e idf_four_tasks`.
2. Виконайте `Wokwi: Select Config File` і виберіть [`wokwi.toml`](wokwi.toml).
3. Відкрийте [`diagram.json`](diagram.json).
4. Виконайте `Wokwi: Start Simulator`.

Щоб записати лог без VS Code, запустіть з кореня репо:

```sh
~/.wokwi/bin/wokwi-cli "dz6.2 (RTOS SCHEDULER)/four_tasks" --timeout 33000 \
  --serial-log-file wokwi.log
```

Для `wokwi-cli` потрібен токен у змінній `WOKWI_CLI_TOKEN`.

## Файли

| Файл | Опис |
|---|---|
| [`main/main.cpp`](main/main.cpp) | прошивка |
| [`sdkconfig.defaults`](sdkconfig.defaults) | налаштування ESP-IDF |
| [`diagram.json`](diagram.json) | плата для Wokwi без зовнішніх компонентів |
| [`wokwi.toml`](wokwi.toml) | конфіг симулятора |
| [`serial.log`](serial.log) | лог плати за 36 с |
| [`wokwi.log`](wokwi.log) | лог симулятора за 33 с |
