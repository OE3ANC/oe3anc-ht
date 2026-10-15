# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ht_companion_connection_tests LANGUAGES C CXX)
target_sources(app PRIVATE src/main.cpp)
target_include_directories(app PRIVATE "${CMAKE_BINARY_DIR}/companion-generated")
add_dependencies(app ht_release_identity)
zephyr_ld_options(-Wl,--wrap=ht_companion_read -Wl,--wrap=ht_companion_write)
