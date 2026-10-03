# SPDX-License-Identifier: GPL-3.0-or-later
zephyr_library_named(ht_channels)
zephyr_library_sources(model.cpp records.cpp)
zephyr_library_compile_options(-fstack-usage)
