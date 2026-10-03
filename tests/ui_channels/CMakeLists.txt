# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ht_ui_channels_tests LANGUAGES C CXX)
target_sources(app PRIVATE src/main.cpp)
zephyr_ld_options(-Wl,--wrap=_ZN2ht19settings_channel_atEjtRNS_7ChannelEPj
                  -Wl,--wrap=_ZN2ht25settings_channel_positionEjjRt
                  -Wl,--wrap=_ZN2ht23settings_channel_numberEtRNS_7ChannelEPj)
