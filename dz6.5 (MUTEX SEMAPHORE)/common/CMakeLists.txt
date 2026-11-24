# Спільний код ДЗ 6.5 як компонент ESP-IDF. Нативна збірка conveyor додає цю папку
# через EXTRA_COMPONENT_DIRS, збірка через PlatformIO - через src/CMakeLists.txt.
# STM32-збірка цей файл не читає: там джерела додає build_src_filter.
idf_component_register(SRCS "press_filter.c" "rate.c" "report.c" INCLUDE_DIRS ".")
