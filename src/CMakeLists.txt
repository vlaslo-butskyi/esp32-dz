# Диспетчер для espidf-env із platformio.ini.
#
# PlatformIO вимагає, щоб у src/ лежав CMakeLists.txt, і реєструє src/ як
# компонент. Але прошивки ДЗ лежать не тут, а в папках своїх варіантів — тих
# самих, що збирає нативний idf.py. Тому цей файл нічого не глобає, а бере
# рівно один main.c, указаний через -DDZ_VARIANT:
#
#   custom_dz_variant = dz2.6 (ESP-IDF DEBOUNCE)/raw_interrupt
#
# Значення приходить змінною середовища DZ_VARIANT, яку виставляє
# tools/pio_idf_variant.py. Через -D не вийшло: ESP-IDF під час expansion
# компонентів включає цей файл окремим процесом `cmake -P`, і визначення з
# командного рядка туди не доходять.
#
# Так обидві збірки — pio і idf.py — читають один і той самий файл, і код
# ніде не дублюється.

if(NOT DEFINED DZ_VARIANT OR DZ_VARIANT STREQUAL "")
    set(DZ_VARIANT "$ENV{DZ_VARIANT}")
endif()

if(DZ_VARIANT STREQUAL "")
    message(FATAL_ERROR
        "DZ_VARIANT не заданий. Цей проєкт збирається лише через espidf-env "
        "із platformio.ini, де вказано custom_dz_variant = dzX.Y (ТЕМА)/варіант. "
        "Для прямої збірки одного варіанту користуйся ./tools/idf.sh")
endif()

set(variant_dir "${CMAKE_SOURCE_DIR}/${DZ_VARIANT}/main")

# Прошивка може бути як C++, так і C: dz2.6 на .cpp, але механізм не прив'язаний
# до мови. Беремо те, що лежить у папці варіанту.
set(variant_main "")
foreach(candidate "${variant_dir}/main.cpp" "${variant_dir}/main.c")
    if(EXISTS "${candidate}")
        set(variant_main "${candidate}")
        break()
    endif()
endforeach()

if(variant_main STREQUAL "")
    message(FATAL_ERROR "немає ні main.cpp, ні main.c у ${variant_dir}")
endif()

message(STATUS "DZ_VARIANT: ${DZ_VARIANT} (${variant_main})")

idf_component_register(SRCS "${variant_main}")
