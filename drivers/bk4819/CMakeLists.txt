# SPDX-License-Identifier: GPL-3.0-or-later
zephyr_library_named(ht_bk4819)
zephyr_library_sources(radio.c dcs.c)
zephyr_library_sources_ifdef(CONFIG_BOARD_C62 gpio.c)
zephyr_include_directories(${ZEPHYR_HT_MODULE_DIR}/drivers/bk4819)
