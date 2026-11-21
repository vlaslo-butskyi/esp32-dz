# ДЗ 6.2 — задачі та планувальник FreeRTOS (RTOS SCHEDULER)

Цей документ описує ДЗ 6.2. Умова: чотири задачі FreeRTOS з пріоритетами Low, Normal, High і Realtime.

| Прошивка | Плата | Опис |
|---|---|---|
| [`four_tasks`](four_tasks/) | ESP32-S3-DevKitC-1 | відповідь на умову, ESP-IDF |
| [`four_tasks_stm32`](four_tasks_stm32/) | WeAct Black Pill, STM32F411 | та сама прошивка на STM32 HAL і FreeRTOS 10.3.1 |
| [`four_tasks_sysview`](four_tasks_sysview/) | ESP32-S3-DevKitC-1 | `four_tasks` під SEGGER SystemView, бонус |

## Як прошивка виконує умову

| Пункт умови | Задача | Функції FreeRTOS |
|---|---|---|
| 1. Sensor Reader, кожні 500 мс | `sensor` | `vTaskDelayUntil` |
| 2. Event Trigger, кожні 3 с | `event` | `vTaskDelay`, `xTaskCreatePinnedToCore` на ESP32, `xTaskCreate` на STM32 |
| 3. Worker Task, одна подія | `worker` | `vTaskDelete(nullptr)` |
| 4. Maintenance Mode, кожні 10 с | `maintenance` | `vTaskSuspend`, `vTaskResume` |

Звичайний `vTaskDelay` у задачі `sensor` не використано. Функція `worker_task` закінчується викликом `vTaskDelete`, а не виходом з функції.
