# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ht_ui_backlight_service_tests LANGUAGES C CXX)
target_sources(app PRIVATE src/main.cpp)
zephyr_ld_options(-Wl,--wrap=_ZN2ht16ui_backend_startEh -Wl,--wrap=_ZN2ht20ui_backend_backlightEh -Wl,--wrap=_ZN2ht16ui_backend_inputERNS_7UiInputE)
