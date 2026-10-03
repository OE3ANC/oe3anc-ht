# SPDX-License-Identifier: Apache-2.0
zephyr_library_named(ht_csk_gpio)
zephyr_library_sources(gpio_csk6.c)
if(CONFIG_GPIO_CSK6)
  message(FATAL_ERROR "Disable vendor GPIO_CSK6 when selecting HT_CSK_GPIO")
endif()
