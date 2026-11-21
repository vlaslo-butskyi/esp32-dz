cmake_minimum_required(VERSION 3.16)
# Компонент main береться з four_tasks: код той самий, SystemView вмикає тільки
# sdkconfig.defaults цієї папки. Окремий файл через SRCS "../../four_tasks/..."
# не підходить: для джерела поза компонентом ninja не записує залежностей, і
# main.cpp не перезбирається після зміни sdkconfig.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../four_tasks/main")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(four_tasks_sysview)
