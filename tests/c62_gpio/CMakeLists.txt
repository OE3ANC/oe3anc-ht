# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ht_c62_gpio_tests LANGUAGES C CXX)
target_sources(app PRIVATE src/main.cpp)
target_include_directories(app PRIVATE src/fakes)
# C source is included in C++ only to monitor its register operators; accept
# C's implicit void-pointer conversions without changing the imported driver.
target_compile_options(app PRIVATE -fpermissive -Wno-unused-function)
