# Корневий CMake-проєкт для збірки ESP-IDF прошивок через PlatformIO
# (framework = espidf). Нативна збірка його не читає: там кожен варіант у
# dz2.6 (ESP-IDF DEBOUNCE)/<варіант>/ сам собі проєкт зі своїм CMakeLists.txt.
#
# Який саме варіант збирати, вирішує src/CMakeLists.txt за -DDZ_VARIANT,
# який приходить із board_build.cmake_extra_args у platformio.ini.
cmake_minimum_required(VERSION 3.16.0)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(dz-espidf)
