# SPDX-License-Identifier: GPL-3.0-or-later
zephyr_library_named(ht_m17_frames)
zephyr_library_sources(frames.cpp)
zephyr_library_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
  $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)
if(CONFIG_HT_M17_MODULATOR OR CONFIG_HT_M17_DEMODULATOR)
  zephyr_library_named(ht_m17_modem)
  zephyr_library_sources_ifdef(CONFIG_HT_M17_MODULATOR modulator.cpp)
  zephyr_library_sources_ifdef(CONFIG_HT_M17_DEMODULATOR demodulator.cpp)
  zephyr_library_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)
endif()
