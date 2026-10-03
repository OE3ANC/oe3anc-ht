/*
 * Copyright (c) 2021 listenai Intelligent Technology (anhui) Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Adapted from the pinned Lisa csk/drivers/adc/adc_csk6.c. Preserve its
 * channel setup and last-of-three sampling, with bounded synchronous reads.
 */
#define DT_DRV_COMPAT listenai_csk_adc

#include <errno.h>
#include <string.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/kernel.h>
#include "Driver_GPADC.h"
#include "gpadc_venus.h"

#define CHANNEL_COUNT 7
#define SAMPLE_COUNT 3
#define READ_TIMEOUT_MS 30

struct ht_adc_data {
    struct k_mutex lock;
    const GPADC_RESOURCES *hal;
};

struct ht_adc_config {
    const struct pinctrl_dev_config *pins;
};

static int ht_adc_channel_setup(const struct device *dev, const struct adc_channel_cfg *cfg) {
    ARG_UNUSED(dev);
    if (!cfg || cfg->channel_id >= CHANNEL_COUNT) {
        return -EINVAL;
    }
    if (cfg->gain != ADC_GAIN_1 || cfg->reference != ADC_REF_INTERNAL ||
        cfg->acquisition_time != ADC_ACQ_TIME_DEFAULT || cfg->differential) {
        return -ENOTSUP;
    }
    return 0;
}

static void ht_adc_stop(const GPADC_RESOURCES *hal) {
    /* No DMA or interrupts own buffers. Avoid HAL_Stop's W1C bitfield RMW. */
    hal->reg->REG_ADC_IMR0.all = UINT32_MAX;
    hal->reg->REG_ADC_IMR1.all = UINT32_MAX;
    hal->reg->REG_ADC_CR.bit.SOFT_TRIG = 0;
    hal->reg->REG_ADC_CR.bit.ADC_EN = 0;
    hal->reg->REG_ADC_ICR0.all = UINT32_MAX;
    hal->info->flags &= ~GPADC_FLAG_POWERED;
}

static int ht_adc_wait(const GPADC_RESOURCES *hal, bool ready, int64_t deadline) {
    while (!(ready ? hal->reg->REG_ADC_IRSR0.bit.ADC_READY_IRSR
                   : hal->reg->REG_ADC_IRSR0.bit.ADC_COMPLETE_IRSR)) {
        if (k_uptime_get() >= deadline) {
            return -ETIMEDOUT;
        }
        k_sleep(K_MSEC(1));
    }
    return k_uptime_get() >= deadline ? -ETIMEDOUT : 0;
}

static unsigned ht_adc_fifo_count(const GPADC_RESOURCES *hal, unsigned channel) {
    return (hal->reg->REG_ADC_FIFO_DATA_CNT0.all >> (4 * channel)) & 0xf;
}

static int ht_adc_clear_fifo(const GPADC_RESOURCES *hal) {
    /* A canceled conversion may have left partial samples, including channels
     * absent from the next sequence. Never assume ADC_EN resets the FIFOs. */
    for (unsigned ch = 0; ch < CHANNEL_COUNT; ++ch) {
        for (unsigned n = 0; n < 15 && ht_adc_fifo_count(hal, ch); ++n) {
            (void)HAL_GPADC_GetValue((void *)hal, BIT(ch) << CSK_GPADC_CHANNEL_SEL_Pos);
        }
        if (ht_adc_fifo_count(hal, ch)) {
            return -EIO;
        }
    }
    return 0;
}

static int ht_adc_read(const struct device *dev, const struct adc_sequence *seq) {
    if (k_is_in_isr()) {
        return -EWOULDBLOCK;
    }
    if (!seq || !seq->channels || (seq->channels & ~BIT_MASK(CHANNEL_COUNT)) ||
        seq->resolution != 11 || !seq->buffer || ((uintptr_t)seq->buffer % __alignof__(uint16_t))) {
        return -EINVAL;
    }
    if (seq->options || seq->oversampling || seq->calibrate) {
        return -ENOTSUP;
    }
    unsigned count = 0;
    for (unsigned ch = 0; ch < CHANNEL_COUNT; ++ch) {
        count += !!(seq->channels & BIT(ch));
    }
    if (seq->buffer_size < count * sizeof(uint16_t)) {
        return -ENOMEM;
    }
    struct ht_adc_data *data = dev->data;
    const struct ht_adc_config *cfg = dev->config;
    int rc = k_mutex_lock(&data->lock, K_MSEC(READ_TIMEOUT_MS));
    if (rc) {
        return rc;
    }
    const GPADC_RESOURCES *hal = data->hal;
    /* DSP startup can overwrite these pins. The driver owns its public config. */
    rc = pinctrl_apply_state(cfg->pins, PINCTRL_STATE_DEFAULT);
    if (rc) {
        goto out;
    }
    const int64_t deadline = k_uptime_get() + READ_TIMEOUT_MS;
    /* HAL_Initialize and HAL_PollForConversion contain unbounded loops.
     * This polling-only owner uses the HAL's public registers/POWERED flag
     * instead, with all IRQ sources masked and a deadline for ADC_READY. */
    hal->reg->REG_ADC_IMR0.all = UINT32_MAX;
    hal->reg->REG_ADC_IMR1.all = UINT32_MAX;
    hal->reg->REG_ADC_ICR0.all = UINT32_MAX;
    hal->reg->REG_ADC_ICR1.all = UINT32_MAX;
    hal->reg->REG_ADC_CR.bit.ADC_EN = 1;
    rc = ht_adc_wait(hal, true, deadline);
    if (rc) {
        goto stop;
    }
    hal->info->flags |= GPADC_FLAG_POWERED;
    const uint32_t control = CSK_GPADC_SAMPLETIME_8 | CSK_GPADC_CLKFREQ_12M |
                             (seq->channels << CSK_GPADC_CHANNEL_SEL_Pos) |
                             CSK_GPADC_INPUT_MODE_Single | CSK_GPADC_VREF_SEL_1_25V |
                             CSK_GPADC_DMA_DISABLE;
    if (HAL_GPADC_Control((void *)hal, control) != CSK_DRIVER_OK ||
        HAL_GPADC_SetTriggerNum((void *)hal, SAMPLE_COUNT) != CSK_DRIVER_OK) {
        rc = -EIO;
        goto stop;
    }
    rc = ht_adc_clear_fifo(hal);
    if (rc || k_uptime_get() >= deadline) {
        rc = rc ? rc : -ETIMEDOUT;
        goto stop;
    }
    hal->reg->REG_ADC_ICR0.all = BIT(CSK_ADC_ISR_COMPLETE_Pos);
    if (HAL_GPADC_Start((void *)hal) != CSK_DRIVER_OK) {
        rc = -EIO;
        goto stop;
    }
    rc = ht_adc_wait(hal, false, deadline);
    if (!rc && (hal->reg->REG_ADC_IRSR0.bit.EOC_ERR_IRSR ||
                hal->reg->REG_ADC_IRSR0.bit.CHANNEL_ERR_IRSR)) {
        rc = -EIO;
    }
    for (unsigned ch = 0; !rc && ch < CHANNEL_COUNT; ++ch) {
        if ((seq->channels & BIT(ch)) && ht_adc_fifo_count(hal, ch) != SAMPLE_COUNT) {
            rc = -EIO;
        }
    }
    if (!rc) {
        uint16_t samples[CHANNEL_COUNT];
        unsigned index = 0;
        hal->reg->REG_ADC_ICR0.all = BIT(CSK_ADC_ISR_COMPLETE_Pos);
        for (unsigned ch = 0; ch < CHANNEL_COUNT; ++ch) {
            if (!(seq->channels & BIT(ch))) {
                continue;
            }
            for (unsigned n = 0; n < SAMPLE_COUNT; ++n) {
                samples[index] =
                    HAL_GPADC_GetValue((void *)hal, BIT(ch) << CSK_GPADC_CHANNEL_SEL_Pos);
            }
            ++index;
        }
        memcpy(seq->buffer, samples, count * sizeof(uint16_t));
    }
stop:
    ht_adc_stop(hal);
out:
    k_mutex_unlock(&data->lock);
    return rc;
}

#ifdef CONFIG_ADC_ASYNC
static int ht_adc_read_async(const struct device *dev, const struct adc_sequence *seq,
                             struct k_poll_signal *signal) {
    ARG_UNUSED(dev);
    ARG_UNUSED(seq);
    ARG_UNUSED(signal);
    return -ENOTSUP;
}
#endif

static int ht_adc_init(const struct device *dev) {
    struct ht_adc_data *data = dev->data;
    const struct ht_adc_config *cfg = dev->config;
    data->hal = GPADC();
    if (!data->hal) {
        return -ENODEV;
    }
    k_mutex_init(&data->lock);
    ht_adc_stop(data->hal);
    return pinctrl_apply_state(cfg->pins, PINCTRL_STATE_DEFAULT);
}

static const struct adc_driver_api ht_adc_api = {
    .channel_setup = ht_adc_channel_setup,
    .read = ht_adc_read,
#ifdef CONFIG_ADC_ASYNC
    .read_async = ht_adc_read_async,
#endif
    /* Preserve the reference transfer function until meter validation. */
    .ref_internal = 3300,
};

#define HT_ADC_DEFINE(n)                                                                           \
    PINCTRL_DT_INST_DEFINE(n);                                                                     \
    static struct ht_adc_data ht_adc_data_##n;                                                     \
    static const struct ht_adc_config ht_adc_config_##n = {                                        \
        .pins = PINCTRL_DT_INST_DEV_CONFIG_GET(n),                                                 \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, ht_adc_init, NULL, &ht_adc_data_##n, &ht_adc_config_##n, POST_KERNEL, \
                          CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &ht_adc_api);

DT_INST_FOREACH_STATUS_OKAY(HT_ADC_DEFINE)
