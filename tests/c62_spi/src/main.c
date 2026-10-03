/* SPDX-License-Identifier: GPL-3.0-or-later */
#define CONFIG_HT_RADIO 1 /* Use the real fault bridge contract with a test observer. */
#include "../../../drivers/spi/spi_csk6.c"
#include <string.h>
#include <zephyr/ztest.h>

static struct csk_spi_data data;
static struct device device = {.data = &data};
static const struct spi_config config = {.frequency = 20000000, .operation = SPI_WORD_SET(8)};

enum behavior { HEALTHY, START_ERROR, TIMEOUT, ABORT_ERROR };
static enum behavior behavior;
static unsigned sends, aborts, faults;
static int reported;
static K_SEM_DEFINE(abort_entered, 0, 1);

void ht_radio_report_fault(int error) {
    ++faults;
    reported = error;
}

int32_t SPI_Control(void *context, uint32_t control, uint32_t arg) {
    ARG_UNUSED(context);
    ARG_UNUSED(arg);
    if (control == CSK_SPI_ABORT_TRANSFER) {
        ++aborts;
        zassert_true(data.failed);
        zassert_equal(faults, 1); // Fault notification precedes a possibly blocked abort.
        const unsigned before = sends;
        csk_spi_isr_callback(CSK_SPI_EVENT_TRANSFER_COMPLETE, (uint32_t)(uintptr_t)&device);
        zassert_equal(sends, before); // Late IRQ cannot start another DMA chunk.
        k_sem_give(&abort_entered);
        return behavior == ABORT_ERROR ? -1 : 0;
    }
    return 0;
}

int32_t SPI_Send(void *context, const void *buffer, uint32_t size) {
    ARG_UNUSED(context);
    ARG_UNUSED(buffer);
    ARG_UNUSED(size);
    ++sends;
    if (behavior == START_ERROR) {
        return -1;
    }
    if (behavior == HEALTHY) {
        csk_spi_isr_callback(CSK_SPI_EVENT_TRANSFER_COMPLETE, (uint32_t)(uintptr_t)&device);
    }
    return 0;
}

int32_t SPI_Receive(void *context, void *buffer, uint32_t size) {
    return SPI_Send(context, buffer, size);
}

int32_t SPI_Transfer(void *context, const void *tx, void *rx, uint32_t size) {
    ARG_UNUSED(rx);
    return SPI_Send(context, tx, size);
}

static void before(void *fixture) {
    ARG_UNUSED(fixture);
    zassert_true((uintptr_t)&device <= UINT32_MAX);
    memset(&data, 0, sizeof(data));
    k_sem_init(&data.ctx.lock, 1, 1);
    k_sem_init(&data.ctx.sync, 0, 1);
    k_sem_reset(&abort_entered);
    sends = aborts = faults = 0;
    reported = 0;
}

static int transfer(void) {
    const uint8_t bytes[] = {0x12, 0x34, 0x56, 0x78};
    const struct spi_buf buffer = {.buf = (void *)bytes, .len = sizeof(bytes)};
    const struct spi_buf_set buffers = {.buffers = &buffer, .count = 1};
    return csk_spi_transceive(&device, &config, &buffers, NULL);
}

ZTEST(c62_spi, test_completion_keeps_normal_path_and_no_abort) {
    behavior = HEALTHY;
    zassert_ok(transfer());
    zassert_equal(sends, 1);
    zassert_equal(aborts, 0);
    zassert_equal(faults, 0);
}

ZTEST(c62_spi, test_start_error_aborts_and_excludes_late_callback_and_reuse) {
    behavior = START_ERROR;
    zassert_equal(transfer(), -EIO);
    zassert_equal(reported, -EIO);
    zassert_equal(aborts, 1);
    const unsigned before = sends;
    behavior = HEALTHY;
    zassert_equal(transfer(), -EIO);
    zassert_equal(sends, before);
}

ZTEST(c62_spi, test_timeout_aborts_before_returning_caller_buffers) {
    behavior = TIMEOUT;
    zassert_equal(transfer(), -ETIMEDOUT);
    zassert_equal(reported, -ETIMEDOUT);
    zassert_equal(aborts, 1);
}

static K_SEM_DEFINE(returned, 0, 1);
K_THREAD_STACK_DEFINE(worker_stack, 2048);
static struct k_thread worker;

static void fail_abort(void *a, void *b, void *c) {
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);
    transfer();
    k_sem_give(&returned);
}

ZTEST(c62_spi, test_failed_abort_retains_callers_context_until_reboot) {
    behavior = ABORT_ERROR;
    k_sem_reset(&returned);
    k_thread_create(&worker, worker_stack, K_THREAD_STACK_SIZEOF(worker_stack), fail_abort, NULL,
                    NULL, NULL, 6, 0, K_NO_WAIT);
    zassert_ok(k_sem_take(&abort_entered, K_SECONDS(1)));
    zassert_equal(k_sem_take(&returned, K_MSEC(20)), -EAGAIN);
    zassert_true(data.failed);
    // Test HAL never owns real DMA; cancel only this test's parked context.
    k_thread_abort(&worker);
}

ZTEST_SUITE(c62_spi, NULL, NULL, before, NULL, NULL);
