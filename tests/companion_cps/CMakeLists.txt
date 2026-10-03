# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ht_companion_cps_tests LANGUAGES C CXX)
target_sources(app PRIVATE src/main.cpp ../../modules/companion/protocol.cpp
  ../../modules/companion/codeplug.cpp ../../modules/companion/cps.cpp)
zephyr_ld_options(-Wl,--wrap=fsync)
