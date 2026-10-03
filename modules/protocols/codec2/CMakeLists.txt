# SPDX-License-Identifier: GPL-3.0-or-later
if(NOT EXISTS "${HT_CODEC2_DIR}/src/codec2_mod.c")
  message(FATAL_ERROR "Set HT_CODEC2_DIR to the pinned Codec2-mod checkout")
endif()
zephyr_library_named(ht_codec2)
zephyr_library_sources(voice.cpp state.c)
foreach(source codec2_mod analysis synthesis nlp lpc interp quantise delta_lsp_cb util
    kiss_fft kiss_fftr)
  zephyr_library_sources(${HT_CODEC2_DIR}/src/${source}.c)
endforeach()
zephyr_library_include_directories(${HT_CODEC2_DIR}/inc)
# The fixed FFTs use caller-owned storage. Reject any unexpected heap path;
# temporary FFT scratch stays on the processing thread's measured stack.
zephyr_library_compile_definitions(KISS_FFT_USE_ALLOCA=1 NDEBUG)
zephyr_library_compile_options(
  -fstack-usage
  $<$<COMPILE_LANGUAGE:C>:-include${CMAKE_CURRENT_SOURCE_DIR}/no-heap.h>
  $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
  $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)
