# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Allow variant builds to stamp a distinct OTA version, e.g.
#   idf.py -DPROJECT_VER=2.0.3-microros build
if(NOT DEFINED PROJECT_VER)
    set(PROJECT_VER "2.0.3")
endif()

# 配置文件统一放在 config/ 下：生成的 sdkconfig 与构建默认值都在这里。
# ⚠️ idf_component.yml / Kconfig.projbuild 是 ESP-IDF 组件元数据，必须跟组件源码在一起。
set(SDKCONFIG "${CMAKE_CURRENT_SOURCE_DIR}/config/sdkconfig")
set(SDKCONFIG_DEFAULTS
    "${CMAKE_CURRENT_SOURCE_DIR}/config/sdkconfig.defaults;${CMAKE_CURRENT_SOURCE_DIR}/config/sdkconfig.defaults.esp32s3")

# 应用组件目录改名为 src/ 后，需要显式告诉 ESP-IDF 去哪里找组件。
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src")

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(xiaozhi)

