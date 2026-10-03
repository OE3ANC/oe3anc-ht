/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#ifdef CONFIG_HT_RADIO
void ht_radio_report_fault(int error);
#else
/* Build-only peripheral probes have no radio service to notify. */
static inline void ht_radio_report_fault(int error) {
    (void)error;
}
#endif
#ifdef __cplusplus
}
#endif
