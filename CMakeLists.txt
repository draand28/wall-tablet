# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

idf_build_set_property(MINIMAL_BUILD ON)
set(EXTRA_COMPONENT_DIRS "components")
# The esp_wifi_remote injected esp_wifi.h requires this symbol when PSRAM is
# enabled, but the component's Kconfig does not reliably emit it.
idf_build_set_property(COMPILE_DEFINITIONS "-DCONFIG_WIFI_RMT_CACHE_TX_BUFFER_NUM=32" APPEND)
project(wall_tablet)
