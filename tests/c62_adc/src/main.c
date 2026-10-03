/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../../../drivers/adc/adc_csk6.c"
#include <zephyr/ztest.h>

static GPADC_RegDef registers;
static GPADC_INFO info;
static GPADC_RESOURCES hal = {.reg = &registers, .info = &info};
static struct ht_adc_data data;
static const pinctrl_soc_pin_t pin = 7;
static const struct pinctrl_state pin_state = {
    .pins = &pin,
    .pin_cnt = 1,
    .id = PINCTRL_STATE_DEFAULT,
};
static const struct pinctrl_dev_config pins = {.states = &pin_state, .state_cnt = 1};
static const struct ht_adc_config config = {.pins = &pins};
static const struct device device = {.data = &data, .config = &config, .api = &ht_adc_api};
static unsigned controls, triggers, starts, reads, pin_calls, discarded;
static int pin_error, control_error, trigger_error, start_error;
static bool stalled, conversion_error, channel_error, fifo_stuck;
static unsigned conversion_count;
static uint32_t channels_seen[CHANNEL_COUNT * SAMPLE_COUNT];
static uint16_t output[CHANNEL_COUNT];
static struct adc_sequence sequence;

void *GPADC(void) {
    return &hal;
}

int pinctrl_configure_pins(const pinctrl_soc_pin_t *p, uint8_t count, uintptr_t reg) {
    ARG_UNUSED(reg);
    zassert_equal_ptr(p, &pin);
    zassert_equal(count, 1);
    ++pin_calls;
    return pin_error;
}

int32_t HAL_GPADC_Control(void *context, uint32_t control) {
    zassert_equal_ptr(context, &hal);
    zassert_true(info.flags & GPADC_FLAG_POWERED);
    zassert_true(registers.REG_ADC_CR.bit.ADC_EN);
    zassert_equal(registers.REG_ADC_IMR0.all, UINT32_MAX);
    zassert_equal(registers.REG_ADC_IMR1.all, UINT32_MAX);
    const uint32_t expected = CSK_GPADC_SAMPLETIME_8 | CSK_GPADC_CLKFREQ_12M |
                              (sequence.channels << CSK_GPADC_CHANNEL_SEL_Pos) |
                              CSK_GPADC_INPUT_MODE_Single | CSK_GPADC_VREF_SEL_1_25V |
                              CSK_GPADC_DMA_DISABLE;
    zassert_equal(control, expected);
    ++controls;
    return control_error;
}

uint32_t HAL_GPADC_SetTriggerNum(void *context, uint32_t count) {
    zassert_equal_ptr(context, &hal);
    zassert_equal(count, 3);
    ++triggers;
    return trigger_error;
}

uint32_t HAL_GPADC_Start(void *context) {
    zassert_equal_ptr(context, &hal);
    zassert_equal(registers.REG_ADC_ICR0.all, BIT(CSK_ADC_ISR_COMPLETE_Pos));
    // Model the W1C performed before starting; an old completion cannot pass.
    registers.REG_ADC_IRSR0.bit.ADC_COMPLETE_IRSR = !stalled;
    registers.REG_ADC_IRSR0.bit.EOC_ERR_IRSR = conversion_error;
    registers.REG_ADC_IRSR0.bit.CHANNEL_ERR_IRSR = channel_error;
    registers.REG_ADC_CR.bit.SOFT_TRIG = 1;
    for (unsigned ch = 0; ch < CHANNEL_COUNT; ++ch) {
        if (sequence.channels & BIT(ch)) {
            registers.REG_ADC_FIFO_DATA_CNT0.all |= conversion_count << (4 * ch);
        }
    }
    ++starts;
    return start_error;
}

uint16_t HAL_GPADC_GetValue(void *context, uint32_t channel) {
    zassert_equal_ptr(context, &hal);
    const unsigned ch = find_lsb_set(channel >> CSK_GPADC_CHANNEL_SEL_Pos) - 1;
    zassert_true(ht_adc_fifo_count(&hal, ch) > 0);
    if (!fifo_stuck) {
        registers.REG_ADC_FIFO_DATA_CNT0.all -= 1U << (4 * ch);
    }
    if (!registers.REG_ADC_CR.bit.SOFT_TRIG) {
        ++discarded;
        return 999; // Partial samples from a failed earlier sequence.
    }
    zassert_true(reads < ARRAY_SIZE(channels_seen));
    channels_seen[reads++] = channel;
    return 2000 + reads;
}

static void before(void *fixture) {
    ARG_UNUSED(fixture);
    memset(&registers, 0, sizeof(registers));
    memset(&info, 0, sizeof(info));
    memset(&data, 0, sizeof(data));
    memset(output, 0x5a, sizeof(output));
    controls = triggers = starts = reads = pin_calls = discarded = 0;
    pin_error = control_error = trigger_error = start_error = 0;
    stalled = conversion_error = channel_error = fifo_stuck = false;
    conversion_count = SAMPLE_COUNT;
    sequence = (struct adc_sequence){.channels = BIT(1) | BIT(2),
                                     .resolution = 11,
                                     .buffer = output,
                                     .buffer_size = sizeof(output)};
    zassert_ok(ht_adc_init(&device));
    registers.REG_ADC_IRSR0.bit.ADC_READY_IRSR = 1;
    pin_calls = 0;
}

static void assert_stopped(void) {
    zassert_false(registers.REG_ADC_CR.bit.ADC_EN);
    zassert_false(registers.REG_ADC_CR.bit.SOFT_TRIG);
    zassert_false(info.flags & GPADC_FLAG_POWERED);
}

static void assert_unchanged(void) {
    for (unsigned n = 0; n < ARRAY_SIZE(output); ++n) {
        zassert_equal(output[n], 0x5a5a);
    }
}

ZTEST(c62_adc, test_channel_order_last_sample_and_pinmux_each_read) {
    zassert_ok(adc_read(&device, &sequence));
    zassert_equal(output[0], 2003);
    zassert_equal(output[1], 2006);
    zassert_equal(reads, 6);
    for (unsigned n = 0; n < 6; ++n) {
        zassert_equal(channels_seen[n], BIT(n < 3 ? 1 : 2) << CSK_GPADC_CHANNEL_SEL_Pos);
    }
    zassert_equal(output[2], 0x5a5a);
    assert_stopped();
    reads = 0;
    zassert_ok(adc_read(&device, &sequence));
    zassert_equal(pin_calls, 2);
    zassert_equal(starts, 2);
    zassert_equal(adc_ref_internal(&device), 3300);
    assert_stopped();
}

ZTEST(c62_adc, test_all_channels_fit_exact_buffer) {
    sequence.channels = BIT_MASK(CHANNEL_COUNT);
    zassert_ok(adc_read(&device, &sequence));
    zassert_equal(reads, CHANNEL_COUNT * SAMPLE_COUNT);
    for (unsigned n = 0; n < CHANNEL_COUNT; ++n) {
        zassert_equal(output[n], 2000 + (n + 1) * SAMPLE_COUNT);
    }
}

ZTEST(c62_adc, test_invalid_requests_do_not_touch_hardware_or_buffer) {
    zassert_equal(ht_adc_read(&device, NULL), -EINVAL);
    sequence.channels = 0;
    zassert_equal(adc_read(&device, &sequence), -EINVAL);
    sequence.channels = BIT(7);
    zassert_equal(adc_read(&device, &sequence), -EINVAL);
    sequence.channels = BIT(1) | BIT(2);
    sequence.buffer_size = 2;
    zassert_equal(adc_read(&device, &sequence), -ENOMEM);
    sequence.buffer_size = sizeof(output);
    sequence.buffer = (uint8_t *)output + 1;
    zassert_equal(adc_read(&device, &sequence), -EINVAL);
    sequence.buffer = NULL;
    zassert_equal(adc_read(&device, &sequence), -EINVAL);
    sequence.buffer = output;
    sequence.resolution = 12;
    zassert_equal(adc_read(&device, &sequence), -EINVAL);
    sequence.resolution = 11;
    sequence.oversampling = 1;
    zassert_equal(adc_read(&device, &sequence), -ENOTSUP);
    sequence.oversampling = 0;
    sequence.calibrate = true;
    zassert_equal(adc_read(&device, &sequence), -ENOTSUP);
    sequence.calibrate = false;
    const struct adc_sequence_options options = {0};
    sequence.options = &options;
    zassert_equal(adc_read(&device, &sequence), -ENOTSUP);
    zassert_equal(pin_calls + controls + triggers + starts + reads, 0);
    assert_unchanged();
}

ZTEST(c62_adc, test_channel_setup_and_async_contract) {
    struct adc_channel_cfg cfg = {.gain = ADC_GAIN_1,
                                  .reference = ADC_REF_INTERNAL,
                                  .acquisition_time = ADC_ACQ_TIME_DEFAULT,
                                  .channel_id = 2};
    zassert_ok(adc_channel_setup(&device, &cfg));
    cfg.channel_id = 7;
    zassert_equal(adc_channel_setup(&device, &cfg), -EINVAL);
    cfg.channel_id = 2;
    cfg.gain = ADC_GAIN_2;
    zassert_equal(adc_channel_setup(&device, &cfg), -ENOTSUP);
    cfg.gain = ADC_GAIN_1;
    cfg.reference = ADC_REF_VDD_1;
    zassert_equal(adc_channel_setup(&device, &cfg), -ENOTSUP);
    cfg.reference = ADC_REF_INTERNAL;
    cfg.differential = 1;
    zassert_equal(adc_channel_setup(&device, &cfg), -ENOTSUP);
    cfg.differential = 0;
    cfg.acquisition_time = ADC_ACQ_TIME(ADC_ACQ_TIME_MICROSECONDS, 10);
    zassert_equal(adc_channel_setup(&device, &cfg), -ENOTSUP);
    zassert_equal(adc_read_async(&device, &sequence, NULL), -ENOTSUP);
}

ZTEST(c62_adc, test_ready_timeout_is_bounded_stops_and_retry_works) {
    registers.REG_ADC_IRSR0.bit.ADC_READY_IRSR = 0;
    const int64_t start = k_uptime_get();
    zassert_equal(adc_read(&device, &sequence), -ETIMEDOUT);
    zassert_true(k_uptime_get() - start < 150);
    zassert_equal(controls, 0);
    assert_stopped();
    assert_unchanged();
    registers.REG_ADC_IRSR0.bit.ADC_READY_IRSR = 1;
    zassert_ok(adc_read(&device, &sequence));
}

ZTEST(c62_adc, test_stale_completion_timeout_and_retry) {
    stalled = true;
    registers.REG_ADC_IRSR0.bit.ADC_COMPLETE_IRSR = 1;
    const int64_t start = k_uptime_get();
    zassert_equal(adc_read(&device, &sequence), -ETIMEDOUT);
    zassert_true(k_uptime_get() - start < 150);
    zassert_equal(reads, 0);
    assert_stopped();
    assert_unchanged();
    stalled = false;
    zassert_ok(adc_read(&device, &sequence));
    zassert_equal(discarded, 6);
    zassert_equal(output[0], 2003);
}

ZTEST(c62_adc, test_partial_fifo_and_unselected_channel_drained_before_sampling) {
    registers.REG_ADC_FIFO_DATA_CNT0.all = (2U << 4) | (1U << 24);
    zassert_ok(adc_read(&device, &sequence));
    zassert_equal(discarded, 3);
    zassert_equal(output[0], 2003);
    zassert_equal(output[1], 2006);
    zassert_equal(registers.REG_ADC_FIFO_DATA_CNT0.all, 0);
}

ZTEST(c62_adc, test_stuck_fifo_and_incomplete_conversion_preserve_output) {
    registers.REG_ADC_FIFO_DATA_CNT0.all = 1U << 4;
    fifo_stuck = true;
    zassert_equal(adc_read(&device, &sequence), -EIO);
    zassert_equal(discarded, 15);
    zassert_equal(starts, 0);
    assert_stopped();
    assert_unchanged();
    fifo_stuck = false;
    conversion_count = 2;
    zassert_equal(adc_read(&device, &sequence), -EIO);
    zassert_equal(reads, 0);
    assert_stopped();
    assert_unchanged();
    conversion_count = 3;
    zassert_ok(adc_read(&device, &sequence));
    zassert_equal(output[0], 2003);
}

ZTEST(c62_adc, test_pinctrl_and_hal_failures_preserve_output) {
    pin_error = -EIO;
    zassert_equal(adc_read(&device, &sequence), -EIO);
    zassert_equal(controls, 0);
    pin_error = 0;
    control_error = -1;
    zassert_equal(adc_read(&device, &sequence), -EIO);
    control_error = 0;
    trigger_error = -1;
    zassert_equal(adc_read(&device, &sequence), -EIO);
    trigger_error = 0;
    start_error = -1;
    zassert_equal(adc_read(&device, &sequence), -EIO);
    start_error = 0;
    conversion_error = true;
    zassert_equal(adc_read(&device, &sequence), -EIO);
    conversion_error = false;
    channel_error = true;
    zassert_equal(adc_read(&device, &sequence), -EIO);
    zassert_equal(reads, 0);
    assert_stopped();
    assert_unchanged();
}

static K_SEM_DEFINE(locked, 0, 1);
static K_SEM_DEFINE(unlock, 0, 1);
K_THREAD_STACK_DEFINE(lock_stack, 1024);
static struct k_thread lock_thread;

static void hold_lock(void *a, void *b, void *c) {
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);
    k_mutex_lock(&data.lock, K_FOREVER);
    k_sem_give(&locked);
    k_sem_take(&unlock, K_FOREVER);
    k_mutex_unlock(&data.lock);
}

ZTEST(c62_adc, test_competing_read_cannot_touch_hardware) {
    k_sem_reset(&locked);
    k_sem_reset(&unlock);
    k_thread_create(&lock_thread, lock_stack, K_THREAD_STACK_SIZEOF(lock_stack), hold_lock, NULL,
                    NULL, NULL, 6, 0, K_NO_WAIT);
    zassert_ok(k_sem_take(&locked, K_SECONDS(1)));
    const int64_t start = k_uptime_get();
    zassert_equal(adc_read(&device, &sequence), -EAGAIN);
    zassert_true(k_uptime_get() - start < 150);
    zassert_equal(pin_calls + controls + starts, 0);
    assert_unchanged();
    k_sem_give(&unlock);
    zassert_ok(k_thread_join(&lock_thread, K_SECONDS(1)));
    zassert_ok(adc_read(&device, &sequence));
}

ZTEST_SUITE(c62_adc, NULL, NULL, before, NULL, NULL);
