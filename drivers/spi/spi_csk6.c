/*
 * SPDX-License-Identifier: Apache-2.0
 * Adapted from csk f097b7f9dfe4aba9fae08af787230e023ee21aa0.
 * Preserve DMA setup; abort failed transfers before releasing caller buffers.
 */

#define DT_DRV_COMPAT listenai_csk_spi

#include <assert.h>
#include <errno.h>
#include <ht/fault.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(csk6_spi, CONFIG_SPI_LOG_LEVEL);

#include "spi_context.h"

#include "Driver_SPI.h"
#include "IOMuxManager.h"
#include "spi.h"

#define SPI0_LABEL "SPI_0"
#define SPI1_LABEL "SPI_1"

#define DEV_CFG(dev) ((const struct csk_spi_config *const)(dev)->config)
#define DEV_DATA(dev) ((struct csk_spi_data *const)(dev)->data)

typedef void *spi_driver_ctx_t;

struct csk_spi_data {
    struct spi_context ctx;
    size_t current_transfer_len;
    spi_driver_ctx_t driver_context;
    bool failed;
};

struct csk_spi_config {
    void (*irq_config_func)(const struct device *dev);
    /* SPI alternate configuration */
    const struct pinctrl_dev_config *pcfg;
    uint32_t reg;
};

static void *spi_csk_get_ctx(const struct device *dev) {
    const struct csk_spi_config *config = dev->config;

    LOG_INF("SPI REG ADDR:0x%x", config->reg);
#ifdef CONFIG_SOC_CSK6
    if (config->reg == SPI0_BASE) {
        return SPI0();
    }
#endif
    if (config->reg == SPI1_BASE) {
        return SPI1();
    }

    LOG_ERR("ERR SPI REG ADDR:0x%x\n", config->reg);

    return NULL;
}

static int csk_spi_configure(const struct device *dev, const struct spi_config *spi_cfg) {
    struct csk_spi_data *data = dev->data;

    if (spi_context_configured(&data->ctx, spi_cfg)) {
        return 0;
    }

    uint32_t spi_op_mode = (SPI_OP_MODE_GET(spi_cfg->operation) == SPI_OP_MODE_MASTER)
                               ? CSK_SPI_MODE_MASTER
                               : CSK_SPI_MODE_SLAVE;

    uint32_t spi_sample_mode = 0;
    bool spi_cpol_mode = SPI_MODE_GET(spi_cfg->operation) & SPI_MODE_CPOL;
    bool spi_cpha_mode = SPI_MODE_GET(spi_cfg->operation) & SPI_MODE_CPHA;
    if (spi_cpol_mode) {     /* cpol = active low */
        if (spi_cpha_mode) { /* cpha = second edge */
            spi_sample_mode = CSK_SPI_CPOL1_CPHA1;
        } else { /* cpha = first edge */
            spi_sample_mode = CSK_SPI_CPOL1_CPHA0;
        }
    } else {                 /* cpol = active high */
        if (spi_cpha_mode) { /* cpha = second edge */
            spi_sample_mode = CSK_SPI_CPOL0_CPHA1;
        } else { /* cpha = first edge */
            spi_sample_mode = CSK_SPI_CPOL0_CPHA0;
        }
    }
    uint32_t spi_word_size = SPI_WORD_SIZE_GET(spi_cfg->operation);
    uint32_t spi_word_size_mask = CSK_SPI_DATA_BITS(spi_word_size);
    uint32_t spi_bits_order =
        (spi_cfg->operation & SPI_TRANSFER_LSB) ? CSK_SPI_LSB_MSB : CSK_SPI_MSB_LSB;

    /* C62 reference uses DMA in both directions. */
    uint32_t spi_dma_en_mask = CSK_SPI_TXIO_DMA | CSK_SPI_RXIO_DMA;

    int r = SPI_Control(data->driver_context,
                        spi_op_mode | spi_sample_mode | spi_word_size_mask | spi_bits_order |
                            spi_dma_en_mask,
                        spi_cfg->frequency);

    data->ctx.config = spi_cfg;

    if (r != 0) {
        LOG_ERR("SPI_Control failed. r:%d.", r);
        return -EIO;
    }

    return 0;
}

static int32_t csk_spi_hal_transceive(const struct device *dev) {
    struct csk_spi_data *data = dev->data;
    struct spi_context *ctx = &data->ctx;
    size_t chunk_len = spi_context_max_continuous_chunk(ctx);
    int32_t r = -EIO;

    if (chunk_len == 0) {
        spi_context_cs_control(&data->ctx, false);
        spi_context_complete(ctx, dev, 0);
        return 0;
    }

    /* Resize the transaction item num since spi driver limitation */
    uint32_t real_trans_size;
    if (ctx->tx_buf == NULL) {
        real_trans_size = chunk_len;
    } else {
        real_trans_size = (chunk_len > MAX_TRANCNT) ? MAX_TRANCNT : chunk_len;
    }
    uint32_t key = irq_lock();
    data->current_transfer_len = real_trans_size;
    if (ctx->tx_buf != NULL && ctx->rx_buf != NULL) {
        r = SPI_Transfer(data->driver_context, ctx->tx_buf, ctx->rx_buf, real_trans_size);
        if (r != 0) {
            LOG_ERR("SPI_Transfer failed. r:%d", r);
        }
    } else if (ctx->tx_buf != NULL) {
        r = SPI_Send(data->driver_context, ctx->tx_buf, real_trans_size);
        if (r != 0) {
            LOG_ERR("SPI_Send failed. r:%d", r);
        }
    } else if (ctx->rx_buf != NULL) {
        r = SPI_Receive(data->driver_context, ctx->rx_buf, real_trans_size);
        if (r != 0) {
            LOG_ERR("SPI_Receive failed. r:%d", r);
        }
    } else {
        /* should not go into here */
    }
    irq_unlock(key);

    return r != 0 ? -EIO : 0;
}

static int transceive(const struct device *dev, const struct spi_config *spi_cfg,
                      const struct spi_buf_set *tx_bufs, const struct spi_buf_set *rx_bufs,
                      spi_callback_t cb, void *userdata) {
    struct csk_spi_data *data = dev->data;
    uint8_t dw = SPI_WORD_SIZE_GET(spi_cfg->operation);

    if (dw != 8 && dw != 16 && dw != 32) {
        LOG_ERR("Unsupported data width(%d bit).", dw);
        return -ENOTSUP;
    }

    if (spi_cfg->operation & (SPI_LINES_DUAL | SPI_LINES_QUAD | SPI_LINES_OCTAL)) {
        LOG_ERR("Unsupported line config.");
        return -ENOTSUP;
    }

    dw /= 8;

#if CONFIG_ASSERT == 1 || defined(CONFIG_SPI_LOG_LEVEL_DBG)
    if (tx_bufs != NULL) {
        size_t c = tx_bufs->count;
        const struct spi_buf *buf = tx_bufs->buffers;
        while (buf && c) {
#if CONFIG_ASSERT == 1
            __ASSERT(buf->len % dw == 0, "tx buffer len(%d) is not match data width(%d byte)",
                     buf->len, dw);
#else
            if (buf->len % dw != 0) {
                LOG_ERR("tx buffer len(%d) is not match data width(%d byte)", buf->len, dw);
                return -EINVAL;
            }
#endif
            buf++;
            c--;
        }
    }

    if (rx_bufs != NULL) {
        size_t c = rx_bufs->count;
        const struct spi_buf *buf = rx_bufs->buffers;
        while (buf && c) {
#if CONFIG_ASSERT == 1
            __ASSERT(buf->len % dw == 0, "rx buffer len(%d) is not match data width(%d byte)",
                     buf->len, dw);
#else
            if (buf->len % dw != 0) {
                LOG_ERR("rx buffer len(%d) is not match data width(%d byte)", buf->len, dw);
                return -EINVAL;
            }
#endif
            buf++;
            c--;
        }
    }
#endif

    spi_context_lock(&data->ctx, (cb != NULL), cb, userdata, spi_cfg);
    if (data->failed) {
        spi_context_release(&data->ctx, -EIO);
        return -EIO;
    }
    int ret = csk_spi_configure(dev, spi_cfg);
    if (ret) {
        goto out;
    }
    spi_context_buffers_setup(&data->ctx, tx_bufs, rx_bufs, dw);
    spi_context_cs_control(&data->ctx, true);
    ret = csk_spi_hal_transceive(dev);
    if (!ret) {
        ret = spi_context_wait_for_completion(&data->ctx);
    }
#ifdef CONFIG_SPI_SLAVE
    SPI_DEV *hal_spi = data->driver_context;
    if (spi_context_is_slave(&data->ctx)) {
        ret = hal_spi->info->xfer.rx_cnt;
    }
#endif /* CONFIG_SPI_SLAVE */

out:
    if (ret < 0) {
        /* Stop callback-driven chunks before quiescing DMA. The HAL abort
         * waits for DMA before clearing its buffers; it may block, so keep
         * interrupts enabled for radio/PTT threads while it runs.
         */
        const unsigned int key = irq_lock();
        data->failed = true;
        irq_unlock(key);
        ht_radio_report_fault(ret);
        if (SPI_Control(data->driver_context, CSK_SPI_ABORT_TRANSFER, 0)) {
            /* Caller buffers must remain alive if quiescence is unproven.
             * During runtime the separate radio owner can still shut down.
             * During POST_KERNEL init the application has not started yet.
             */
            LOG_ERR("SPI abort failed; reboot required");
            for (;;) {
                k_sleep(K_FOREVER);
            }
        }
        spi_context_cs_control(&data->ctx, false);
    }
    spi_context_release(&data->ctx, ret);
    return ret;
}

static int csk_spi_transceive(const struct device *dev, const struct spi_config *spi_cfg,
                              const struct spi_buf_set *tx_bufs,
                              const struct spi_buf_set *rx_bufs) {
    return transceive(dev, spi_cfg, tx_bufs, rx_bufs, NULL, NULL);
}

#ifdef CONFIG_SPI_ASYNC
static int csk_spi_transceive_async(const struct device *dev, const struct spi_config *spi_cfg,
                                    const struct spi_buf_set *tx_bufs,
                                    const struct spi_buf_set *rx_bufs, spi_callback_t cb,
                                    void *userdata) {
    return transceive(dev, spi_cfg, tx_bufs, rx_bufs, cb, userdata);
}
#endif /* CONFIG_SPI_ASYNC */

static int csk_spi_release(const struct device *dev, const struct spi_config *spi_cfg) {
    struct csk_spi_data *data = dev->data;
    spi_context_unlock_unconditionally(&data->ctx);
    return 0;
}

static void csk_spi_isr_callback(uint32_t event, uint32_t usr_param) {
    const struct device *dev = (const struct device *)usr_param;
    struct csk_spi_data *data = dev->data;
    if (data->failed) {
        return;
    }
    const struct spi_config *config = data->ctx.config;

    uint8_t dw = SPI_WORD_SIZE_GET(config->operation) / 8;
    if (event & CSK_SPI_EVENT_TRANSFER_COMPLETE) {
        spi_context_update_tx(&data->ctx, dw, data->current_transfer_len);
        spi_context_update_rx(&data->ctx, dw, data->current_transfer_len);
        csk_spi_hal_transceive(dev);
    }
#ifdef CONFIG_SOC_LS_ADSP_CSK6
    else if (event & CSK_SPI_EVENT_DMA_COMPLETE) {
        spi_context_update_tx(&data->ctx, dw, data->current_transfer_len);
        spi_context_update_rx(&data->ctx, dw, data->current_transfer_len);
        csk_spi_hal_transceive(dev);
    }
#endif /* end of CONFIG_SOC_LS_ADSP_CSK6 */

    /* TODO: add transfer error processing */
}

static void csk_zephyr_spi_isr(const struct device *dev) {
    struct csk_spi_data *data = dev->data;
    SPI_DEV *hal_spi = data->driver_context;

    if (hal_spi->irq_handler != NULL) {
        hal_spi->irq_handler();
    }
}

static const struct spi_driver_api csk_spi_driver_api = {
    .transceive = csk_spi_transceive,
#ifdef CONFIG_SPI_ASYNC
    .transceive_async = csk_spi_transceive_async,
#endif
    .release = csk_spi_release,
};

static int spi_csk_init(const struct device *dev) {
    const struct csk_spi_config *config = DEV_CFG(dev);
    struct csk_spi_data *data = DEV_DATA(dev);

    /* Get device handle */
    data->driver_context = spi_csk_get_ctx(dev);

    /* Fixme: rework the SPI_Initialize(), let the user parameter become void* type */
    SPI_Initialize(data->driver_context, csk_spi_isr_callback, (uint32_t)dev);
    SPI_PowerControl(data->driver_context, CSK_POWER_FULL);

    pinctrl_apply_state(config->pcfg, PINCTRL_STATE_DEFAULT);

    /* IRQ_CONNECT and irq_enable */
    config->irq_config_func(dev);

    int err = spi_context_cs_configure_all(&data->ctx);
    if (err < 0) {
        return err;
    }

    spi_context_unlock_unconditionally(&data->ctx);

    return 0;
}

#define SPI_DEVICE_INIT(n)                                                                         \
                                                                                                   \
    static void spi_csk_config_func_##n(const struct device *dev) {                                \
                                                                                                   \
        IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority), csk_zephyr_spi_isr,                 \
                    DEVICE_DT_INST_GET(n), 0);                                                     \
                                                                                                   \
        irq_enable(DT_INST_IRQN(n));                                                               \
    }                                                                                              \
                                                                                                   \
    PINCTRL_DT_INST_DEFINE(n);                                                                     \
                                                                                                   \
    static const struct csk_spi_config csk_spi_config_##n = {                                      \
        .irq_config_func = spi_csk_config_func_##n,                                                \
        .pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),                                                 \
        .reg = DT_REG_ADDR(DT_DRV_INST(n)),                                                        \
    };                                                                                             \
                                                                                                   \
    static struct csk_spi_data csk_spi_data_##n = {                                                \
        SPI_CONTEXT_INIT_LOCK(csk_spi_data_##n, ctx),                                              \
        SPI_CONTEXT_INIT_SYNC(csk_spi_data_##n, ctx),                                              \
        SPI_CONTEXT_CS_GPIOS_INITIALIZE(DT_DRV_INST(n), ctx)};                                     \
                                                                                                   \
    DEVICE_DT_INST_DEFINE(n, &spi_csk_init, NULL, &csk_spi_data_##n, &csk_spi_config_##n,          \
                          POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &csk_spi_driver_api);

DT_INST_FOREACH_STATUS_OKAY(SPI_DEVICE_INIT)
