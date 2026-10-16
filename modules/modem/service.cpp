// SPDX-License-Identifier: GPL-3.0-or-later
#include <ht/modem_service.hpp>
#include <ht/radio.hpp>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

namespace ht {
namespace {
struct Packet {
    modem::Frame frame;
    uint32_t epoch;
};
K_MSGQ_DEFINE(tx_queue, sizeof(Packet), 4, 4);
K_MSGQ_DEFINE(rx_queue, sizeof(Packet), 4, 4);
K_MUTEX_DEFINE(mutex);
atomic_t enabled, epoch, transmitting;
ModemStatus status;
modem::Frame pending;
int64_t next_slot;
uint32_t random_state = 1;
uint32_t revision;
AudioSession audio_session;
bool audio_tx;
int64_t heartbeat;

#ifdef CONFIG_HT_AUDIO
bool current(uint32_t job, uint32_t generation) {
    k_mutex_lock(&mutex, K_FOREVER);
    const bool valid = job == revision && generation == modem_epoch() && modem_enabled();
    if (valid) {
        heartbeat = k_uptime_get();
    }
    k_mutex_unlock(&mutex);
    return valid;
}

modem::Transmitter transmitter;
modem::Receiver receiver;
void worker(void *, void *, void *) {
    uint32_t previous = 0;
    int16_t samples[480];
    while (true) {
        k_mutex_lock(&mutex, K_FOREVER);
        const uint32_t job = revision, generation = modem_epoch();
        const auto session = audio_session;
        const auto settings = status.settings;
        const bool tx = audio_tx;
        if (job != previous && session.id && modem_enabled()) {
            if (tx) {
                transmitter.reset();
                if (!transmitter.start(pending, settings.txdelay, settings.txtail,
                                       int(settings.level) * 32767 / 100)) {
                    status.error = -EINVAL;
                    audio_session = {};
                }
            } else {
                receiver.reset();
            }
        }
        k_mutex_unlock(&mutex);
        if (job == previous || !session.id || !modem_enabled()) {
            k_sleep(K_MSEC(5));
            continue;
        }
        previous = job;
        int error = 0;
        while (current(job, generation)) {
            if (tx) {
                const size_t count = transmitter.render(samples, 480);
                if (!count) {
                    error = audio_drain(session, AudioOutput::Radio, 500);
                    break;
                }
                // Preserve this rendered block until accepted or cancelled.
                do {
                    error = audio_write(session, AudioOutput::Radio, samples, count, 20);
                } while ((error == -EAGAIN || error == -ETIMEDOUT) && current(job, generation));
            } else {
                error = audio_read(session, AudioInput::Radio, samples, 480, 20);
                if (error == -EAGAIN || error == -ETIMEDOUT) {
                    continue;
                }
                if (!error && current(job, generation)) {
                    Packet packet;
                    packet.epoch = generation;
                    for (int16_t sample : samples) {
                        if (receiver.sample(sample, packet.frame)) {
                            k_mutex_lock(&mutex, K_FOREVER);
                            if (current(job, generation)) {
                                if (k_msgq_put(&rx_queue, &packet, K_NO_WAIT)) {
                                    ++status.dropped;
                                } else {
                                    ++status.received;
                                }
                            }
                            k_mutex_unlock(&mutex);
                        }
                    }
                }
            }
            if (error) {
                break;
            }
        }
        k_mutex_lock(&mutex, K_FOREVER);
        if (current(job, generation)) {
            if (error && error != -ECANCELED) {
                status.error = error;
            }
            status.finished = tx && !error;
        }
        k_mutex_unlock(&mutex);
    }
}

K_THREAD_DEFINE(modem_worker, 4096, worker, nullptr, nullptr, nullptr, 7, 0, 0);
#endif
} // namespace

void modem_disable() {
    if (atomic_cas(&enabled, 1, 0)) {
        atomic_clear(&transmitting);
        atomic_inc(&epoch);
    }
}

bool modem_enabled() {
    return atomic_get(&enabled);
}

uint32_t modem_epoch() {
    return atomic_get(&epoch);
}

void modem_enable() {
    k_mutex_lock(&mutex, K_FOREVER);
    k_msgq_purge(&tx_queue);
    k_msgq_purge(&rx_queue);
    status.error = 0;
    status.finished = false;
    next_slot = 0;
    // Serial/menu arrival timing decorrelates devices without enabling a new
    // hardware entropy peripheral. This is CSMA jitter, never security randomness.
    random_state ^= k_cycle_get_32();
    if (!random_state) {
        random_state = 1;
    }
    atomic_inc(&epoch);
    atomic_set(&enabled, 1);
    k_mutex_unlock(&mutex);
}

void modem_settings(const ModemSettings &settings) {
    k_mutex_lock(&mutex, K_FOREVER);
    status.settings = settings;
    k_mutex_unlock(&mutex);
}

ModemStatus modem_status() {
    k_mutex_lock(&mutex, K_FOREVER);
    if (audio_session.id && modem_enabled() && !status.finished &&
        k_uptime_get() - heartbeat > 1000) {
        status.error = -ETIMEDOUT;
    }
    auto result = status;
    result.enabled = modem_enabled();
    result.transmitting = modem_tx_requested();
    result.queued = result.enabled ? k_msgq_num_used_get(&tx_queue) : 0;
    k_mutex_unlock(&mutex);
    return result;
}

void modem_host(const modem::KissMessage &message, uint32_t generation) {
    k_mutex_lock(&mutex, K_FOREVER);
    if (generation != modem_epoch() || !modem_enabled()) {
        k_mutex_unlock(&mutex);
        return;
    }
    const uint8_t value = message.frame.bytes[0];
    switch (message.command) {
    case modem::KissCommand::Data: {
        random_state ^= k_cycle_get_32();
        if (!random_state) {
            random_state = 1;
        }
        Packet packet{message.frame, generation};
        if (k_msgq_put(&tx_queue, &packet, K_NO_WAIT)) {
            ++status.dropped;
        }
        break;
    }
    case modem::KissCommand::TxDelay: status.settings.txdelay = value; break;
    case modem::KissCommand::Persistence: status.settings.persistence = value; break;
    case modem::KissCommand::SlotTime: status.settings.slot_time = value; break;
    case modem::KissCommand::TxTail: status.settings.txtail = value; break;
    case modem::KissCommand::FullDuplex: status.settings.full_duplex = value != 0; break;
    case modem::KissCommand::Return: modem_disable(); break;
    }
    k_mutex_unlock(&mutex);
}

void modem_rejected() {
    k_mutex_lock(&mutex, K_FOREVER);
    ++status.rejected;
    k_mutex_unlock(&mutex);
}

bool modem_output(modem::Frame &frame, uint32_t generation) {
    Packet packet;
    while (!k_msgq_get(&rx_queue, &packet, K_NO_WAIT)) {
        if (packet.epoch == generation && generation == modem_epoch() && modem_enabled()) {
            frame = packet.frame;
            return true;
        }
    }
    return false;
}

bool modem_admit(bool carrier) {
    k_mutex_lock(&mutex, K_FOREVER);
    const auto now = k_uptime_get();
    bool admitted = false;
    if (modem_enabled() && !modem_tx_requested() && k_msgq_num_used_get(&tx_queue)) {
        if (!status.settings.full_duplex && carrier) {
            next_slot = now + (status.settings.slot_time ? status.settings.slot_time * 10 : 10);
        } else if (now >= next_slot) {
            next_slot = now + (status.settings.slot_time ? status.settings.slot_time * 10 : 10);
            random_state ^= random_state << 13;
            random_state ^= random_state >> 17;
            random_state ^= random_state << 5;
            if (status.settings.full_duplex ||
                (random_state & 255) <= status.settings.persistence) {
                Packet packet;
                while (!k_msgq_get(&tx_queue, &packet, K_NO_WAIT)) {
                    if (packet.epoch == modem_epoch()) {
                        pending = packet.frame;
                        status.finished = false;
                        atomic_set(&transmitting, 1);
                        admitted = true;
                        break;
                    }
                }
            }
        }
    }
    k_mutex_unlock(&mutex);
    return admitted;
}

bool modem_tx_requested() {
    return modem_enabled() && atomic_get(&transmitting) && radio_power_requested() &&
           !radio_pending_fault();
}

void modem_complete() {
    k_mutex_lock(&mutex, K_FOREVER);
    if (status.finished) {
        ++status.transmitted;
    }
    atomic_clear(&transmitting);
    k_mutex_unlock(&mutex);
}

void modem_audio_stop() {
    k_mutex_lock(&mutex, K_FOREVER);
    ++revision;
    audio_session = {};
    k_mutex_unlock(&mutex);
}

int modem_receive(AudioSession session) {
    k_mutex_lock(&mutex, K_FOREVER);
    ++revision;
    audio_session = session;
    audio_tx = false;
    status.finished = false;
    heartbeat = k_uptime_get();
    k_mutex_unlock(&mutex);
    return 0;
}

int modem_transmit(AudioSession session) {
    k_mutex_lock(&mutex, K_FOREVER);
    ++revision;
    audio_session = session;
    audio_tx = true;
    status.finished = false;
    heartbeat = k_uptime_get();
#ifndef CONFIG_HT_AUDIO
    status.finished = true; // Emulator models packet ownership, not an RF/audio waveform.
#endif
    k_mutex_unlock(&mutex);
    return 0;
}
} // namespace ht
