# SPDX-License-Identifier: GPL-3.0-or-later
zephyr_library_named(ht_power)
zephyr_library_sources(service.cpp)
if(CONFIG_BOARD_C62)
  zephyr_library_sources(${ZEPHYR_HT_MODULE_DIR}/backends/c62/power.cpp)
elseif(CONFIG_HT_EMULATOR)
  zephyr_library_sources(${ZEPHYR_HT_MODULE_DIR}/backends/emulator/power.cpp)
endif()
