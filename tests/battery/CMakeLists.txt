# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ht_battery_tests LANGUAGES C CXX)
target_sources(app PRIVATE src/main.cpp ../../backends/c62/battery_decode.cpp)
