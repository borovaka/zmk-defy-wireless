# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20.0)
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/defy_test.cmake)

# The longest values the VERSION command carries, 32, 140 and 72 bytes, which
# the module's CMakeLists.txt writes into <dygma/defy_build_info.h>; src/main.c
# expects them.
set(DEFY_BUILD_MODULE_VERSION v0.1.0-12-g2826cb2abcdef01-dirty)
string(REPEAT o 39 owner)
string(REPEAT r 100 repository)
set(DEFY_BUILD_SOURCE ${owner}/${repository})
set(DEFY_BUILD_COMMIT 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef01234567)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(defy_updater_test)

target_sources(app PRIVATE src/main.c ${DEFY_ROOT}/updater/defy_updater.c
                           ${DEFY_ROOT}/bootloader/defy_update_guard.c)
