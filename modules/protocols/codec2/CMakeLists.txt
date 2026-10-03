# SPDX-License-Identifier: GPL-3.0-or-later
# Match the reference's Codec2 source pin and floating-point KISS FFT path.
# Build-local allocation fixes leave the dependency checkout unchanged.
if(NOT EXISTS "${HT_CODEC2_DIR}/src/codec2.c")
  message(FATAL_ERROR "Set HT_CODEC2_DIR to the pinned Codec2 checkout")
endif()
set(patched_dir ${CMAKE_CURRENT_BINARY_DIR}/patched)
file(MAKE_DIRECTORY ${patched_dir})
# Generate the pinned library's version header instead of picking up a host
# system installation or an untracked header from the reference build.
set(CODEC2_VERSION_MAJOR 0)
set(CODEC2_VERSION_MINOR 9)
set(CODEC2_VERSION_PATCH 2)
set(CODEC2_VERSION "0.9.2")
configure_file(${HT_CODEC2_DIR}/cmake/version.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/include/codec2/version.h)
foreach(source codec2.c nlp.c)
  configure_file(${HT_CODEC2_DIR}/src/${source} ${patched_dir}/${source} COPYONLY)
endforeach()
find_program(patch_program patch REQUIRED)
execute_process(COMMAND ${patch_program} --batch --fuzz=0 -p1
  -d ${patched_dir} -i ${CMAKE_CURRENT_SOURCE_DIR}/allocation.patch
  RESULT_VARIABLE patch_result OUTPUT_QUIET ERROR_VARIABLE patch_error)
if(NOT patch_result EQUAL 0)
  message(FATAL_ERROR "Codec2 allocation patch failed: ${patch_error}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS allocation.patch)

zephyr_library_named(ht_codec2)
zephyr_library_sources(voice.cpp allocator.c ${patched_dir}/codec2.c ${patched_dir}/nlp.c)
foreach(source dump lpc phase quantise postfilter codec2_fft lsp sine interp
    kiss_fft kiss_fftr newamp1 codebook codebookd pack codebooknewamp1 codebooknewamp1_energy)
  zephyr_library_sources(${HT_CODEC2_DIR}/src/${source}.c)
endforeach()
zephyr_library_include_directories(${CMAKE_CURRENT_BINARY_DIR}/include ${HT_CODEC2_DIR}/src)
zephyr_library_compile_definitions(CODEC2_MODE_EN_DEFAULT=0 CODEC2_MODE_3200_EN=1
  FREEDV_MODE_EN_DEFAULT=0 KISS_FFT_USE_ALLOCA=1 M_PI=3.14159265358979323846)
zephyr_library_compile_options(
  -fstack-usage
  $<$<COMPILE_LANGUAGE:C>:-include${CMAKE_CURRENT_SOURCE_DIR}/allocator.h>
  $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
  $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)
