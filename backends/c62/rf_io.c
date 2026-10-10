/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "rf_io.h"
#include <errno.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#ifdef CONFIG_HT_RESOURCE_DIAGNOSTICS
#include <IOMuxManager.h>
#include <gpt.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(ht_c62_rf, LOG_LEVEL_INF);
#endif

static const struct pwm_dt_spec apc = PWM_DT_SPEC_GET(DT_NODELABEL(pwm_rf_apc));
static const struct gpio_dt_spec speaker = GPIO_DT_SPEC_GET(DT_NODELABEL(speaker_enable), gpios);
static const struct gpio_dt_spec dtmf = GPIO_DT_SPEC_GET(DT_NODELABEL(dtmf_enable), gpios);
static const struct gpio_dt_spec green = GPIO_DT_SPEC_GET(DT_NODELABEL(ledrx), gpios);

int c62_apc(uint8_t duty_percent) {
    if (duty_percent > 100) {
        return -EINVAL;
    }
    // Preserve the tested 1 kHz APC; requested milliwatts are not measured RF power.
    const int error = pwm_set_dt(&apc, PWM_USEC(1000), PWM_USEC(10 * duty_percent));
#ifdef CONFIG_HT_RESOURCE_DIAGNOSTICS
    if (duty_percent) {
        // Read only configuration registers: distinguish a changing request
        // from a stopped timer or lost PA pin mux on the actual radio.
        const GPT_RESOURCES *gpt = GPT0();
        uint32_t mux = 0;
        const int mux_error = IOMuxManager_GetConfig(CSK_IOMUX_PAD_A, 3, &mux);
        LOG_INF("APC duty=%u%% error=%d reload=%08x ctrl=%08x clock=%08x", duty_percent, error,
                gpt->reg->CHx_RELOAD[apc.channel], gpt->reg->CHx_CTRL[apc.channel],
                gpt->reg->CHx_CLK_CTRL[apc.channel]);
        LOG_INF("APC count=%08x timers=%08x PA3-mux=%u error=%d", gpt->reg->CHx_CNT[apc.channel],
                gpt->reg->CH_TIMER_ENABLE, mux, mux_error);
    }
#endif
    return error;
}

int c62_speaker(bool enabled) {
    return gpio_pin_set_dt(&speaker, enabled);
}

int c62_receive_led(bool enabled) {
    return gpio_pin_set_dt(&green, enabled);
}

int c62_rf_io_init(void) {
    if (!device_is_ready(apc.dev) || !gpio_is_ready_dt(&speaker) || !gpio_is_ready_dt(&dtmf) ||
        !gpio_is_ready_dt(&green)) {
        return -ENODEV;
    }
    // Try every inactive output even if another operation fails.
    const int power_error = c62_apc(0);
    const int speaker_error = gpio_pin_configure_dt(&speaker, GPIO_OUTPUT_INACTIVE);
    const int dtmf_error = gpio_pin_configure_dt(&dtmf, GPIO_OUTPUT_INACTIVE);
    const int led_error = gpio_pin_configure_dt(&green, GPIO_OUTPUT_INACTIVE);
    return power_error     ? power_error
           : speaker_error ? speaker_error
           : dtmf_error    ? dtmf_error
                           : led_error;
}
