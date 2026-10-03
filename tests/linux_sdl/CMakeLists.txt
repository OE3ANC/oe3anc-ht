# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ht_linux_sdl_tests LANGUAGES C CXX)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
target_sources(app PRIVATE src/main.cpp ../../backends/linux/sdl.cpp)
target_include_directories(app PRIVATE ${SDL2_INCLUDE_DIRS})
target_link_options(native_simulator INTERFACE ${SDL2_LDFLAGS})
