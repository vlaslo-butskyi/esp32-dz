# Спільний код ДЗ 6.4 як компонент ESP-IDF. Нативна збірка safe_fsm додає цю папку
# через EXTRA_COMPONENT_DIRS, збірка через PlatformIO - через src/CMakeLists.txt.
# STM32-збірка цей файл не читає: там джерела додає build_src_filter.
idf_component_register(SRCS "button_fsm.c" "safe_fsm.c" INCLUDE_DIRS ".")
