# Pre-script для stm32cube-env, яким потрібен FreeRTOS (ДЗ 6.1 і далі).
#
# Ядро FreeRTOS лежить у пакеті framework-stm32cubef4
# (Middlewares/Third_Party/FreeRTOS, версія 10.3.1), але PlatformIO його не
# збирає: stm32cube.py будує тільки HAL, CMSIS і USB-бібліотеки ST. Цей скрипт
# додає шляхи до заголовків і компілює ядро разом із прошивкою.
#
# В env достатньо двох рядків:
#   extra_scripts = pre:tools/pio_stm32_freertos.py
#   custom_freertos_config = dz6.1 (RTOS TASKS)/three_tasks_stm32
# custom_freertos_config - папка, де лежить FreeRTOSConfig.h.
#
# Порт ARM_CM3, хоч чип - Cortex-M4F. PlatformIO збирає F411 без
# -mfloat-abi=hard, а порт ARM_CM4F без апаратного FPU не компілюється
# (#error про __VFP_FP__). Порт CM3 на M4 працює, якщо задачі не рахують у
# float: тоді регістри FPU зберігати не треба.

import os

Import("env")

config_dir = env.GetProjectOption("custom_freertos_config", "").strip()
if not config_dir:
    print("error: custom_freertos_config не заданий для env %s" % env["PIOENV"])
    env.Exit(1)

project_dir = env.subst("$PROJECT_DIR")
config_dir = os.path.join(project_dir, config_dir)
if not os.path.isfile(os.path.join(config_dir, "FreeRTOSConfig.h")):
    print("error: немає FreeRTOSConfig.h у %s" % config_dir)
    env.Exit(1)

framework = env.PioPlatform().get_package_dir("framework-stm32cubef4")
rtos = os.path.join(framework, "Middlewares", "Third_Party", "FreeRTOS", "Source")
port = os.path.join(rtos, "portable", "GCC", "ARM_CM3")

env.Append(CPPPATH=[config_dir, os.path.join(rtos, "include"), port])

env.BuildSources(
    os.path.join("$BUILD_DIR", "FreeRTOS"),
    rtos,
    src_filter=[
        "-<*>",
        "+<tasks.c>",
        "+<list.c>",
        "+<queue.c>",
        "+<portable/GCC/ARM_CM3/port.c>",
        "+<portable/MemMang/heap_4.c>",
    ],
)
