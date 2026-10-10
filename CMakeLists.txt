# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20.0)
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
if(BOARD STREQUAL "c62")
  include(cmake/dsp-image.cmake)
endif()
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(oe3anc_ht LANGUAGES C CXX)
target_include_directories(app PRIVATE include)
target_sources(app PRIVATE app/main.cpp)
if(CONFIG_BOARD_C62)
  target_sources(app PRIVATE backends/c62/memory.c)
  target_compile_options(app PRIVATE -fstack-usage)
  zephyr_compile_definitions(PSRAM_CHIP_SIZE=4)
endif()
target_compile_options(app PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti>)
if(CONFIG_BOARD_C62 AND CONFIG_HT_COMPANION)
  set(bundle_tag_args)
  if(DEFINED HT_RELEASE_TAG AND NOT HT_RELEASE_TAG STREQUAL "")
    list(APPEND bundle_tag_args --tag "${HT_RELEASE_TAG}")
  endif()
  add_custom_target(ht_firmware_bundle ALL
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/firmware_bundle.py"
            --build-dir "${CMAKE_BINARY_DIR}" ${bundle_tag_args}
    BYPRODUCTS "${CMAKE_BINARY_DIR}/zephyr/firmware-bundle.json"
    DEPENDS zephyr_final ht_dsp_firmware ht_release_identity
    VERBATIM)
endif()
