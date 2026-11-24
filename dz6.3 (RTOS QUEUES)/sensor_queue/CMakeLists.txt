cmake_minimum_required(VERSION 3.16)
# Облік повідомлень і фільтр натискань - спільні з STM32-версією, лежать у ../common.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../common")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sensor_queue)
