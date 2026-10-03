# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(ht_companion_renderer LANGUAGES C CXX)
set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 14)
get_filename_component(HT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
file(GLOB_RECURSE lvgl_sources CONFIGURE_DEPENDS "${HT_LVGL_DIR}/src/*.c")
add_library(ht_lvgl STATIC ${lvgl_sources})
target_include_directories(ht_lvgl PUBLIC "${HT_ROOT}/include" "${HT_LVGL_DIR}/src")
target_compile_definitions(ht_lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE=1 LV_KCONFIG_IGNORE=1)
add_executable(ht-ui runtime.cpp "${HT_ROOT}/modules/ui/view.cpp"
  "${HT_ROOT}/modules/ui/theme.cpp" "${HT_ROOT}/modules/ui/presentation_wire.cpp")
target_link_libraries(ht-ui PRIVATE ht_lvgl)
target_compile_options(ht-ui PRIVATE -fno-exceptions -fno-rtti -Wall -Wextra)
if(EMSCRIPTEN)
  set_target_properties(ht-ui PROPERTIES SUFFIX ".mjs")
  target_compile_options(ht_lvgl PRIVATE -O2)
  target_compile_options(ht-ui PRIVATE -O2)
  target_link_options(ht-ui PRIVATE --no-entry -O2
    -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=web,node -sFILESYSTEM=0
    -sINITIAL_MEMORY=8388608 -sSTACK_SIZE=65536 -sALLOW_MEMORY_GROWTH=0
    "-sEXPORTED_FUNCTIONS=['_ht_ui_start','_ht_ui_apply','_ht_ui_tick','_ht_ui_buffer','_ht_ui_pixels']"
    "-sEXPORTED_RUNTIME_METHODS=['HEAPU8','HEAPU16']")
else()
  # Native parity probe links the same renderer/runtime and consumes wire files.
  target_sources(ht-ui PRIVATE "${HT_ROOT}/tests/companion/render.cpp")
endif()
