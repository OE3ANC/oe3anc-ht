# SPDX-License-Identifier: GPL-3.0-or-later
zephyr_library_named(ht_companion)
zephyr_library_sources(protocol.cpp service.cpp ptt.cpp)
if(CONFIG_HT_UI)
  zephyr_library_sources(ui.cpp keys.cpp)
endif()
if(CONFIG_HT_CODEPLUG_STORAGE AND CONFIG_HT_UI)
  zephyr_library_sources(codeplug.cpp cps.cpp)
endif()
zephyr_library_compile_options(-fstack-usage)
set(release_dir "${CMAKE_BINARY_DIR}/companion-generated")
set(release_tag_args)
if(DEFINED HT_RELEASE_TAG AND NOT HT_RELEASE_TAG STREQUAL "")
  list(APPEND release_tag_args --tag "${HT_RELEASE_TAG}")
endif()
add_custom_target(ht_release_identity ALL
  COMMAND "${Python3_EXECUTABLE}" "${ZEPHYR_HT_MODULE_DIR}/tools/companion_contract.py" --check
  COMMAND "${Python3_EXECUTABLE}" "${ZEPHYR_HT_MODULE_DIR}/tools/companion_release.py"
          --output "${release_dir}" ${release_tag_args}
  BYPRODUCTS "${release_dir}/ht/release.hpp" "${release_dir}/release.json" "${release_dir}/release.mjs"
  VERBATIM)
add_dependencies(ht_companion ht_release_identity)
zephyr_library_include_directories("${release_dir}")
