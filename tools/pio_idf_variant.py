# Pre-script для espidf-env із platformio.ini.
#
# Передає вибраний варіант ДЗ у CMake через змінні середовища, а не через
# -D у board_build.cmake_extra_args: ESP-IDF під час expansion компонентів
# включає src/CMakeLists.txt окремим процесом `cmake -P`, і визначення з
# командного рядка туди не доходять, а середовище — доходить.
#
# В env достатньо одного рядка:
#   custom_dz_variant = dz2.6 (ESP-IDF DEBOUNCE)/raw_interrupt
#
# Звідси беруться обидві речі: який main.c збирати (DZ_VARIANT читає
# src/CMakeLists.txt) і який sdkconfig.defaults застосувати — той самий файл,
# що використовує нативна збірка через tools/idf.sh.

import os

Import("env")

variant = env.GetProjectOption("custom_dz_variant", "").strip()
if not variant:
    print("error: custom_dz_variant не заданий для env %s" % env["PIOENV"])
    env.Exit(1)

project_dir = env.subst("$PROJECT_DIR")
main_dir = os.path.join(project_dir, variant, "main")
defaults = os.path.join(project_dir, variant, "sdkconfig.defaults")

# Прошивка може бути .cpp або .c — вибір робить src/CMakeLists.txt, тут лише
# перевірка, що варіант узагалі існує.
if not any(os.path.isfile(os.path.join(main_dir, name))
           for name in ("main.cpp", "main.c")):
    print("error: немає ні main.cpp, ні main.c у %s" % main_dir)
    env.Exit(1)

if not os.path.isfile(defaults):
    print("error: немає %s" % defaults)
    env.Exit(1)

os.environ["DZ_VARIANT"] = variant
os.environ["SDKCONFIG_DEFAULTS"] = defaults

print("ESP-IDF variant: %s" % variant)
