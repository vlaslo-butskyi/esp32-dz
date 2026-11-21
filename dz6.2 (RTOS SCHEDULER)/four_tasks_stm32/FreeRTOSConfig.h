// Налаштування FreeRTOS для ДЗ 6.2 на STM32F411 (Black Pill).
// Відмінності від ДЗ 6.1: м'ютекс для UART, vTaskSuspend/vTaskResume, vTaskDelete.
#ifndef DZ62_FREERTOS_CONFIG_H
#define DZ62_FREERTOS_CONFIG_H

#include <stdint.h>
extern uint32_t SystemCoreClock;  // CMSIS, оновлює HAL_RCC_ClockConfig()

#define configUSE_PREEMPTION                    1
#define configUSE_TIME_SLICING                  1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 1
#define configCPU_CLOCK_HZ                      (SystemCoreClock)
#define configTICK_RATE_HZ                      ((TickType_t)1000)  // тік 1 мс
#define configMAX_PRIORITIES                    5                   // 0 idle, 1..4 задачі
#define configMINIMAL_STACK_SIZE                ((uint16_t)128)     // у словах по 4 байти
#define configTOTAL_HEAP_SIZE                   ((size_t)(8 * 1024))
#define configMAX_TASK_NAME_LEN                 16
#define configUSE_16_BIT_TICKS                  0
#define configIDLE_SHOULD_YIELD                 1

#define configUSE_MUTEXES                       1  // UART спільний для чотирьох задач
#define configUSE_COUNTING_SEMAPHORES           0
#define configUSE_TIMERS                        0
#define configUSE_CO_ROUTINES                   0
#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     0
#define configSUPPORT_DYNAMIC_ALLOCATION        1
#define configSUPPORT_STATIC_ALLOCATION         0

// Переповнення стеку і нестача купи ведуть в обробники в main.cpp.
#define configCHECK_FOR_STACK_OVERFLOW          2
#define configUSE_MALLOC_FAILED_HOOK            1

#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_vTaskSuspend                    1  // Maintenance Mode
#define INCLUDE_vTaskDelete                     1  // Worker Task
#define INCLUDE_xTaskGetSchedulerState          1  // потрібна SysTick_Handler у main.cpp

// Пріоритети переривань Cortex-M: у STM32F4 4 біти, 0 - найвищий.
#define configPRIO_BITS                         4
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY      15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5
#define configKERNEL_INTERRUPT_PRIORITY \
  (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY \
  (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

void rtos_assert_failed(void);
#define configASSERT(x) if ((x) == 0) { rtos_assert_failed(); }

// Обробники з таблиці векторів startup_stm32f411xe.s. SysTick_Handler пише
// main.cpp, бо SysTick потрібен і HAL, і FreeRTOS.
#define vPortSVCHandler    SVC_Handler
#define xPortPendSVHandler PendSV_Handler

#endif
