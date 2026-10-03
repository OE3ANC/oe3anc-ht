# SPDX-License-Identifier: Apache-2.0
zephyr_library_named(ht_csk_spi)
zephyr_library_sources(spi_csk6.c)
zephyr_library_include_directories(${ZEPHYR_BASE}/drivers/spi)
if(CONFIG_SPI_CSK6)
  message(FATAL_ERROR "Disable vendor SPI_CSK6 when selecting HT_CSK_SPI")
endif()
