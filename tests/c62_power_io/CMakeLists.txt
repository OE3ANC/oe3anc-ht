# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ht_c62_power_io_tests LANGUAGES C CXX)
target_sources(app PRIVATE src/main.c src/backend.cpp ../../backends/c62/power.cpp)
target_include_directories(app PRIVATE ../../backends/c62 ../../drivers/bk4819)
