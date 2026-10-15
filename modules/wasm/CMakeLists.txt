# SPDX-License-Identifier: GPL-3.0-or-later
zephyr_library_named(ht_wasm)
zephyr_library_sources(service.cpp)
if(CONFIG_BOARD_C62)
  set(HT_WASM_CLANG "${ZEPHYR_HT_MODULE_DIR}/.deps/ci/emsdk/upstream/bin/clang"
      CACHE FILEPATH "Clang with wasm32 backend and wasm-ld")
  set(wasm_output "${PROJECT_BINARY_DIR}/wasm-tests")
  add_custom_command(OUTPUT "${wasm_output}/wasm_programs.h"
    COMMAND ${PYTHON_EXECUTABLE} ${ZEPHYR_HT_MODULE_DIR}/examples/wasm-poc/build_tests.py
      --clang ${HT_WASM_CLANG} --output ${wasm_output}
    DEPENDS ${ZEPHYR_HT_MODULE_DIR}/examples/wasm-poc/build_tests.py
      ${ZEPHYR_HT_MODULE_DIR}/examples/wasm-poc/test_program.c
    VERBATIM)
  zephyr_library_sources(${ZEPHYR_HT_MODULE_DIR}/backends/c62/wasm.cpp
    "${wasm_output}/wasm_programs.h")
  zephyr_library_include_directories(${wasm_output})
endif()
