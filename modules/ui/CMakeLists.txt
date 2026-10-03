# SPDX-License-Identifier: GPL-3.0-or-later
# Compile the pinned LVGL sources with one project configuration on both
# Zephyr baselines. Their built-in LVGL glue expects different LVGL releases.
zephyr_library_named(ht_lvgl)
file(GLOB_RECURSE lvgl_sources CONFIGURE_DEPENDS ${HT_LVGL_DIR}/src/*.c)
zephyr_library_sources(${lvgl_sources})
zephyr_include_directories(${HT_LVGL_DIR}/src ${ZEPHYR_HT_MODULE_DIR}/include)
zephyr_compile_definitions(LV_CONF_INCLUDE_SIMPLE=1 LV_KCONFIG_IGNORE=1)
if(CONFIG_HT_UI)
  zephyr_library_named(ht_ui)
  zephyr_library_compile_options(-fstack-usage)
  zephyr_library_sources(model.cpp menu.cpp status.cpp diagnostic.cpp home.cpp browse.cpp channel.cpp bank.cpp vfo.cpp quick.cpp limit.cpp backlight.cpp lock.cpp input.cpp presentation.cpp presentation_wire.cpp view.cpp service.cpp theme.cpp text.cpp)
  if(CONFIG_HT_C62_UI)
    zephyr_library_sources(${ZEPHYR_HT_MODULE_DIR}/backends/c62/ui.cpp)
    if(CONFIG_BOARD_C62)
      zephyr_library_sources(${ZEPHYR_HT_MODULE_DIR}/backends/c62/ui_io.c)
    endif()
  else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED sdl2)
    zephyr_library_sources(${ZEPHYR_HT_MODULE_DIR}/backends/linux/sdl.cpp
      ${ZEPHYR_HT_MODULE_DIR}/backends/emulator/ui.cpp)
    zephyr_library_include_directories(${SDL2_INCLUDE_DIRS})
    zephyr_library_compile_options(${SDL2_CFLAGS_OTHER})
    # native_sim first links a relocatable kernel; SDL belongs on its final runner.
    target_link_options(native_simulator INTERFACE ${SDL2_LDFLAGS})
  endif()
endif()
