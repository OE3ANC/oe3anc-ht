# SPDX-License-Identifier: GPL-3.0-or-later
zephyr_library_named(ht_modes)
zephyr_library_sources_ifdef(CONFIG_HT_FM fm.cpp)
zephyr_library_sources_ifdef(CONFIG_HT_M17_MODE m17.cpp)
zephyr_library_compile_options(-fstack-usage)
