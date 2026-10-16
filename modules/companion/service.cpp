// SPDX-License-Identifier: GPL-3.0-or-later
#include <errno.h>
#include <ht/companion.h>
#include <ht/companion_protocol.hpp>
#include <ht/companion_ptt.hpp>
#include <ht/radio.hpp>
#include <ht/release.hpp>
#include <zephyr/kernel.h>
#ifdef CONFIG_HT_UI
#include <ht/companion_ui.hpp>
#include <ht/companion_keys.hpp>
#include <ht/ui_input.hpp>
#ifdef CONFIG_BOARD_C62
static ht::companion::UiTransfer ui __attribute__((section(".psramdata_section")));
#else
static ht::companion::UiTransfer ui;
#endif
#define UI_CAPABILITY (ht::companion::CAP_UI_SNAPSHOT | ht::companion::CAP_UI_KEYS)
#else
#define UI_CAPABILITY 0
#endif
#if defined(CONFIG_HT_CODEPLUG_STORAGE) && defined(CONFIG_HT_UI)
#include <ht/companion_cps.hpp>
#ifdef CONFIG_BOARD_C62
static ht::companion::Cps cps __attribute__((section(".psram_section")));
#else
static ht::companion::Cps cps;
#endif
#define CPS_CAPABILITY ht::companion::CAP_CPS
#else
#define CPS_CAPABILITY 0
#endif
static void product(void *, const ht::companion::Frame &request, ht::companion::Frame &response) {
    if (request.type == ht::companion::MSG_PTT_PRESS ||
        request.type == ht::companion::MSG_PTT_KEEP ||
        request.type == ht::companion::MSG_PTT_RELEASE) {
        ht::companion::handle_ptt(request, response);
        return;
    }
#ifdef CONFIG_HT_UI
    if (request.type == ht::companion::MSG_UI_KEY ||
        request.type == ht::companion::MSG_UI_KEYS_CLEAR ||
        request.type == ht::companion::MSG_UI_KEYS_KEEP) {
        ht::companion::handle_keys(request, response);
        return;
    }
    if (request.type == ht::companion::MSG_UI_POLL || request.type == ht::companion::MSG_UI_CHUNK) {
        ui.handle(request, response);
        return;
    }
#endif
#if defined(CONFIG_HT_CODEPLUG_STORAGE) && defined(CONFIG_HT_UI)
    cps.handle(request, response);
#else
    response.size = 1;
    response.payload[0] = ht::companion::STATUS_UNSUPPORTED;
#endif
}

namespace ht {
namespace companion {
#ifdef CONFIG_BOARD_C62
static Session session(HT_RELEASE_IDENTITY, "c62", CPS_CAPABILITY | UI_CAPABILITY | CAP_PTT,
                       product);
#else
static Session session(HT_RELEASE_IDENTITY, "emulator", CPS_CAPABILITY | UI_CAPABILITY | CAP_PTT,
                       product);
#endif
static Decoder decoder;
static Frame request, response;
static uint8_t wire[MAX_ENCODED];

static void reset_product() {
    // Revoke controller intent as well as transfer state when a session is lost.
    radio_remote_session(0);
#ifdef CONFIG_HT_UI
    ui.reset();
    ui_remote_session(0);
#endif
#if defined(CONFIG_HT_CODEPLUG_STORAGE) && defined(CONFIG_HT_UI)
    cps.reset();
#endif
}

static void run(void *, void *, void *) {
    uint8_t bytes[256];
#ifdef CONFIG_HT_MODEM
    modem::KissParser kiss;
    modem::KissMessage message;
    modem::Frame packet;
    uint8_t kiss_wire[2 * modem::max_frame_size + 3];
    uint32_t epoch = modem_epoch();
    int64_t quiet_until = 0;
#endif
    reset_product();
    while (true) {
        const int64_t now = k_uptime_get();
        if (session.expire(now)) {
            reset_product();
        }
#ifdef CONFIG_HT_MODEM
        if (epoch != modem_epoch()) {
            epoch = modem_epoch();
            session.reset();
            decoder.reset();
            kiss.reset();
            reset_product();
            // Drain old-protocol bytes through a quiet interval, including an
            // ISR-delivered suffix. Neither parser sees transition leftovers.
            quiet_until = now + 20;
        }
#endif
        const int count = ht_companion_read(bytes, sizeof(bytes));
#ifdef CONFIG_HT_MODEM
        if (quiet_until) {
            if (count > 0) {
                quiet_until = now + 20;
            } else if (now >= quiet_until) {
                quiet_until = 0;
            }
            k_sleep(K_MSEC(1));
            continue;
        }
        if (modem_enabled()) {
            if (count < 0) {
                kiss.reset();
                modem_rejected();
            }
            for (int i = 0; i < count && epoch == modem_epoch(); ++i) {
                const auto result = kiss.feed(bytes[i], message);
                if (result == modem::KissResult::Message) {
                    modem_host(message, epoch);
                } else if (result == modem::KissResult::Rejected) {
                    modem_rejected();
                }
            }
            if (modem_output(packet, epoch)) {
                const size_t length = modem::kiss_encode(packet.bytes, packet.size, kiss_wire,
                                                         sizeof(kiss_wire));
                for (size_t offset = 0; offset < length && epoch == modem_epoch();) {
                    const size_t count = length - offset > 256 ? 256 : length - offset;
                    if (ht_companion_write(kiss_wire + offset, count, 100) != int(count)) {
                        break;
                    }
                    offset += count;
                }
            }
            k_sleep(K_MSEC(1));
            continue;
        }
#endif
        if (count < 0) {
            session.reset();
            decoder.reset();
            reset_product();
        } else {
            for (int i = 0; i < count; ++i) {
#ifdef CONFIG_HT_MODEM
                if (epoch != modem_epoch()) {
                    break;
                }
#endif
                if (!decoder.feed(bytes[i], k_uptime_get(), request)) {
                    continue;
                }
                const auto previous = session.identity();
                if (!session.handle(request, k_uptime_get(), response)) {
                    continue;
                }
                if (previous != session.identity()) {
                    reset_product();
                    radio_remote_session(session.identity());
#ifdef CONFIG_HT_UI
                    ui_remote_session(session.identity());
#endif
                }
                const size_t length = encode(response, wire, sizeof(wire));
                // Every frame starts with zero to discard a failed prefix.
                if (length && !modem_enabled()) {
                    (void)ht_companion_write(wire, length, 100);
                }
            }
        }
        k_sleep(K_MSEC(1));
    }
}

K_THREAD_DEFINE(companion_thread, 4096, run, nullptr, nullptr, nullptr, 8, 0, 0);
} // namespace companion
} // namespace ht
