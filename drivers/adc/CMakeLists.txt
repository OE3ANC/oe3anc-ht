# SPDX-License-Identifier: Apache-2.0
zephyr_library_named(ht_csk_adc)
zephyr_library_sources(adc_csk6.c)
if(CONFIG_ADC_CSK6)
  message(FATAL_ERROR "Disable vendor ADC_CSK6 when selecting HT_CSK_ADC")
endif()
