cmake_minimum_required(VERSION 3.16)
# Фільтр брязкоту, швидкість і текст звіту - спільні з STM32-версією, лежать у ../common.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../common")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(conveyor)
