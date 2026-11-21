# ДЗ 6.2 — four_tasks_stm32 (RTOS SCHEDULER)

Прошивка для **WeAct Black Pill V3.1** зі **STM32F411CEU6**. Чотири задачі FreeRTOS з різними пріоритетами пишуть рядки в USART1. Задачі й пріоритети такі самі, як у прошивці [`four_tasks`](../four_tasks/) для ESP32-S3.

| Задача з умови | Ім'я в коді | Пріоритет | Значення | Що робить |
|---|---|---|---|---|
| Sensor Reader | `sensor` | Normal | 2 | кожні 500 мс виводить `[Sensor] Data read...` |
| Event Trigger | `event` | Low | 1 | кожні 3 с створює задачу `worker` |
| Worker Task | `worker` | High | 3 | виводить `[Worker] Processing event...` і видаляє себе |
| Maintenance Mode | `maintenance` | Realtime | 4 | кожні 10 с зупиняє `sensor` на 2 с |

## Підключення

Плата не має послідовного порту для монітора. Причини описано в [ДЗ 2.8](../../dz2.8%20%28STM32%20BUTTON%29/button_modes_uart/). Тому прошивка пише лог у USART1, а перехідник USB-UART CH340 передає його в комп'ютер.

| Black Pill | Куди |
|---|---|
| SWD: 3V3, SWDIO, SWCLK, GND | ST-LINK V2 |
| PA9 (USART1_TX) | RXD перехідника CH340 |
| GND | GND перехідника CH340 |

TXD перехідника не підключайте. Прошивка тільки передає дані.

## Відмінності від ESP32-версії

| | ESP32-S3 | STM32F411 |
|---|---|---|
| створення задач | `xTaskCreatePinnedToCore`, ядро 1 | `xTaskCreate`, бо ядро одне |
| запуск планувальника | ESP-IDF до виклику `app_main()` | `main()` викликає `vTaskStartScheduler()` |
| вивід | `printf` у консоль ESP-IDF | `log_line()`: `HAL_UART_Transmit` під м'ютексом |
| стек | 3072 байти | 256 слів, тобто 1024 байти |
| FreeRTOS | 10.5.1, частина ESP-IDF | 10.3.1 з пакета `framework-stm32cubef4` |
| налаштування FreeRTOS | `sdkconfig.defaults` | [`FreeRTOSConfig.h`](FreeRTOSConfig.h) |

Функції задач `sensor_task`, `event_task`, `worker_task` і `maintenance_task` такі самі, як в ESP32-версії. Тільки `printf` замінено на `log_line()`.

## Як працює код

Файл: [`main.cpp`](main.cpp)

### Вивід у UART

`HAL_UART_Transmit()` повертається тільки після передачі останнього байта. На 115200 бод рядок займає від 2,8 до 4,7 мс. Якщо дві задачі викличуть цю функцію одночасно, рядки перемішаються. Тому `log_line()` бере м'ютекс `uart_mutex`:

```cpp
int n = snprintf(line, sizeof line, "%7lu  ",
                 (unsigned long)(xTaskGetTickCount() * portTICK_PERIOD_MS));
...
xSemaphoreTake(uart_mutex, portMAX_DELAY);
HAL_UART_Transmit(&huart1, (uint8_t *)line, n, HAL_MAX_DELAY);
xSemaphoreGive(uart_mutex);
```

`log_line()` читає номер тіку до м'ютекса. Тому лог показує момент події, а не момент, коли UART став вільним.

Для м'ютекса [`FreeRTOSConfig.h`](FreeRTOSConfig.h) задає `configUSE_MUTEXES 1`. М'ютекс FreeRTOS має успадкування пріоритету. Якщо `maintenance` чекає на UART, задача, яка тримає м'ютекс, тимчасово отримує пріоритет Realtime.

### Задачі

`main()` налаштовує тактування, світлодіод і USART1. Потім `main()` створює м'ютекс і три задачі та запускає планувальник. Задачу `worker` створює `event` під час роботи.

Задачам потрібні функції, які [`FreeRTOSConfig.h`](FreeRTOSConfig.h) вмикає окремо:

| Макрос | Функція | Задача |
|---|---|---|
| `INCLUDE_vTaskDelayUntil` | `vTaskDelayUntil` | `sensor`, `maintenance` |
| `INCLUDE_vTaskSuspend` | `vTaskSuspend`, `vTaskResume` | `maintenance` |
| `INCLUDE_vTaskDelete` | `vTaskDelete` | `worker` |

Пояснення до `vTaskDelayUntil` і двох рядків після нього є в [README ESP32-версії](../four_tasks/).

## Лог плати

Перехідник CH340 записав 36 с логу після перезапуску плати. Перезапуск зробив ST-LINK. Фрагмент логу:

```
   3001  [Sensor] Data read...
   3004  [Worker] Processing event...
   ...
   9012  [Worker] Processing event...
   9501  [Sensor] Data read...
  10001  [Maintenance] System paused for 2 seconds.
  12005  [Sensor] Data read...
  12016  [Worker] Processing event...
  12501  [Sensor] Data read...
```

Повний лог: [`serial.log`](serial.log).

| Задача | Моменти в логу | Інтервал |
|---|---|---|
| `sensor` | 501, 1001 ... 9501, потім 12501 ... 19501 | 500 мс |
| `maintenance` | 10001, 20001, 30001 | 10 000 мс |
| `sensor` після паузи | 12005, 22005, 32005 | 2004 мс від рядка `maintenance` |
| `worker` | 3004, 6008, 9012 ... 36048 | 3004 мс |

Лог показує такі факти:
- `sensor` і `maintenance` працюють без зсуву, бо обидві задачі використовують `vTaskDelayUntil`.
- Рядок `worker` зсувається на 4 мс за цикл. За 12 циклів зсув склав 48 мс.
- Пауза `sensor` триває 2004 мс. `maintenance` передає свій рядок 4,7 мс, і тільки потім викликає `vTaskDelay`.
- Протягом паузи `worker` виводить рядки далі. Приклад: рядок 21028.

Зсув `worker` показує дрейф `vTaskDelay`. `event` викликає `vTaskDelay` тільки після роботи `worker`. `worker` передає рядок 3,4 мс, тому кожен цикл триває 3000 мс плюс 4 мс. В ESP32-версії `printf` записує рядок у буфер UART і повертається менш ніж за 1 мс. Тому там зсуву немає.

## Стек

ST-LINK прочитав купу FreeRTOS після 36 с роботи. FreeRTOS заповнює новий стек байтами `0xA5`. Байти, які задача не змінила, показують запас стеку.

| Задача | Стек | Використано |
|---|---|---|
| `sensor`, `maintenance` | 1024 Б | 560 Б |
| `worker`, останній екземпляр | 1024 Б | 544 Б |
| `event` | 1024 Б | 168 Б |
| idle-задача | 512 Б | 80 Б |

Задачі, які викликають `log_line()`, використовують приблизно на 400 Б більше за `event`.

## Помилки

Якщо прошивка потрапила у фатальну помилку, світлодіод плати на PC13 горить постійно. `Error_Handler()` викликають такі події:

| Подія | Обробник |
|---|---|
| `HAL_RCC_OscConfig()`, `HAL_RCC_ClockConfig()` або `HAL_UART_Init()` повернула помилку | функція налаштування |
| `xSemaphoreCreateMutex()` або `xTaskCreate()` у `main()` не створила об'єкт | `main()`, `start()` |
| `configASSERT` спрацював | `rtos_assert_failed()` |
| задача переповнила стек | `vApplicationStackOverflowHook()` |
| купі FreeRTOS не вистачило пам'яті | `vApplicationMallocFailedHook()` |

Якщо `event` не може створити `worker`, прошивка не зупиняється. `event` виводить `[Event] no memory for Worker Task` і пробує знову через 3 с.

## Запуск

1. Підключіть ST-LINK V2 до SWD-роз'єму плати.
2. Підключіть PA9 до RXD перехідника CH340, GND до GND.
3. Прошийте плату:
   ```sh
   pio run -e stm32_four_tasks -t upload
   ```
4. Відкрийте монітор порту:
   ```sh
   pio device monitor -e stm32_four_tasks
   ```

Якщо монітор нічого не показує, перевірте провід на PA9. Провід має йти на RXD перехідника.

## Збірка

| Параметр | Значення |
|---|---:|
| Flash | 12 684 Б |
| RAM | 8732 Б, з них 8192 Б — купа FreeRTOS |

## Файли

| Файл | Опис |
|---|---|
| [`main.cpp`](main.cpp) | прошивка |
| [`FreeRTOSConfig.h`](FreeRTOSConfig.h) | налаштування FreeRTOS |
| [`serial.log`](serial.log) | лог плати за 36 с |
| [`../../tools/pio_stm32_freertos.py`](../../tools/pio_stm32_freertos.py) | скрипт, який додає ядро FreeRTOS у збірку |
