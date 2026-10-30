# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20.0)
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/defy_test.cmake)
# Longer than the 18 bytes the report carries; the module's CMakeLists.txt
# writes it into <dygma/defy_build_info.h>.
set(DEFY_BUILD_MODULE_VERSION v0.1.0-12-g2826cb2-dirty)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(defy_half_version_test)

target_sources(app PRIVATE src/main.c ${DEFY_ROOT}/battery/defy_half_version_protocol.c)
target_include_directories(app PRIVATE ${DEFY_ROOT}/tests/drivers/common)
