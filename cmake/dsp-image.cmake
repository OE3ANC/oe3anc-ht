# SPDX-License-Identifier: GPL-3.0-or-later
set(HT_DSP_IMAGE "${CMAKE_CURRENT_LIST_DIR}/../backends/c62/resources/dsp_firmware.bin"
    CACHE FILEPATH "Original C62 DSP image to patch")
file(SHA256 "${HT_DSP_IMAGE}" dsp_hash)
if(NOT dsp_hash STREQUAL "9579b617f048c646465118f6d0b9e1b765b4d7d811ff9c8649c5754fe45a8903")
  message(FATAL_ERROR "C62 DSP firmware does not match the tested 48 kHz host transport")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(dsp_patch "${CMAKE_CURRENT_LIST_DIR}/../tools/patch_dsp_uart.py")
set(dsp_output "${CMAKE_BINARY_DIR}/zephyr/dsp_firmware.bin")
add_custom_command(
  OUTPUT "${dsp_output}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/zephyr"
  COMMAND "${Python3_EXECUTABLE}" "${dsp_patch}"
          --input "${HT_DSP_IMAGE}" --output "${dsp_output}" --replace-output
  DEPENDS "${HT_DSP_IMAGE}" "${dsp_patch}"
  COMMENT "Generating C62 DSP firmware without UART logging"
  VERBATIM)
add_custom_target(ht_dsp_firmware ALL DEPENDS "${dsp_output}")
