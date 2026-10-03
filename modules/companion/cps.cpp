// SPDX-License-Identifier: GPL-3.0-or-later
#include <errno.h>
#include <ht/companion_codeplug.hpp>
#include <ht/companion_cps.hpp>
#include <ht/ui_companion.hpp>
#include <string.h>
#include <zephyr/sys/byteorder.h>

namespace ht {
namespace companion {
static uint32_t next_operation;

static uint8_t result(int error) {
    switch (error) {
    case 0:
        return STATUS_OK;
    case -EINVAL:
    case -EBADMSG:
    case -EEXIST:
    case -ENOENT:
        return STATUS_INVALID;
    case -ENOTSUP:
        return STATUS_UNSUPPORTED;
    case -ESTALE:
        return STATUS_STALE;
    case -EBUSY:
    case -EAGAIN:
    case -ENOMSG:
        return STATUS_BUSY;
    case -EROFS:
        return STATUS_PROTECTED;
    case -ECANCELED:
        return STATUS_CANCELLED;
    default:
        return STATUS_FAILED;
    }
}

void Cps::reset() {
    phase_ = Phase::Idle;
    token_ = length_ = offset_ = checksum_ = operation_id_ = 0;
}

void Cps::handle(const Frame &request, Frame &response) {
    auto fail = [&](uint8_t status) {
        response.size = 1;
        response.payload[0] = status;
    };
    const auto *payload = request.payload;
    if (request.type == MSG_CPS_READ) {
        if (request.size) {
            fail(STATUS_INVALID);
            return;
        }
        if (phase_ == Phase::Writing ||
            (phase_ == Phase::Accepted && settings_replacement_status().pending)) {
            fail(STATUS_BUSY);
            return;
        }
        const auto before = radio_snapshot();
        settings_copy_codeplug(plug_, baseline_);
        radio_ = radio_snapshot();
        if (before.generation != radio_.generation ||
            before.configuration_revision != radio_.configuration_revision) {
            reset();
            fail(STATUS_BUSY);
            return;
        }
        size_t length = 0;
        const int error = encode_codeplug(plug_, bytes_, sizeof(bytes_), length);
        if (error || !baseline_.revision) {
            reset();
            fail(result(error ? error : -EBUSY));
            return;
        }
        token_ = request.request;
        length_ = length;
        offset_ = 0;
        checksum_ = crc32(bytes_, length_);
        phase_ = Phase::Reading;
        response.size = 31;
        sys_put_le32(token_, response.payload + 1);
        sys_put_le32(baseline_.revision, response.payload + 5);
        sys_put_le32(radio_.generation, response.payload + 9);
        sys_put_le32(radio_.configuration_revision, response.payload + 13);
        sys_put_le32(length_, response.payload + 17);
        sys_put_le32(checksum_, response.payload + 21);
        sys_put_le32(baseline_.generation, response.payload + 25);
        response.payload[29] = unsigned(baseline_.pending) | (unsigned(baseline_.read_only) << 1);
        response.payload[30] = result(baseline_.save_error);
        return;
    }
    if (request.type < MSG_CPS_READ_CHUNK || request.type > MSG_CPS_CANCEL) {
        fail(STATUS_UNSUPPORTED);
        return;
    }
    if (request.size < 4) {
        fail(STATUS_INVALID);
        return;
    }
    if (sys_get_le32(payload) != token_ || phase_ == Phase::Idle) {
        fail(STATUS_STALE);
        return;
    }
    switch (request.type) {
    case MSG_CPS_READ_CHUNK: {
        if (request.size != 10) {
            fail(STATUS_INVALID);
            return;
        }
        const auto offset = sys_get_le32(payload + 4);
        const auto count = sys_get_le16(payload + 8);
        if (phase_ != Phase::Reading || offset != offset_) {
            fail(STATUS_STALE);
            return;
        }
        if (!count || count > CPS_CHUNK_BYTES || count > length_ - offset_) {
            fail(STATUS_INVALID);
            return;
        }
        response.size = 9 + count;
        sys_put_le32(token_, response.payload + 1);
        sys_put_le32(offset_, response.payload + 5);
        memcpy(response.payload + 9, bytes_ + offset_, count);
        offset_ += count;
        if (offset_ == length_) {
            phase_ = Phase::Ready;
        }
        break;
    }
    case MSG_CPS_WRITE: {
        if (request.size != 12) {
            fail(STATUS_INVALID);
            return;
        }
        if (phase_ != Phase::Ready) {
            fail(STATUS_STALE);
            return;
        }
        const auto length = sys_get_le32(payload + 4);
        if (!length || length > sizeof(bytes_)) {
            fail(STATUS_INVALID);
            return;
        }
        const auto current = radio_snapshot();
        if (settings_status().revision != baseline_.revision ||
            current.generation != radio_.generation ||
            current.configuration_revision != radio_.configuration_revision) {
            reset();
            fail(STATUS_STALE);
            return;
        }
        length_ = length;
        checksum_ = sys_get_le32(payload + 8);
        offset_ = 0;
        token_ = request.request;
        phase_ = Phase::Writing;
        response.size = 5;
        sys_put_le32(token_, response.payload + 1);
        break;
    }
    case MSG_CPS_WRITE_CHUNK: {
        if (request.size <= 8 || request.size > 8 + CPS_CHUNK_BYTES) {
            fail(STATUS_INVALID);
            return;
        }
        if (phase_ != Phase::Writing || sys_get_le32(payload + 4) != offset_) {
            fail(STATUS_STALE);
            return;
        }
        const unsigned count = request.size - 8;
        if (count > length_ - offset_) {
            fail(STATUS_INVALID);
            return;
        }
        memcpy(bytes_ + offset_, payload + 8, count);
        offset_ += count;
        break;
    }
    case MSG_CPS_COMMIT: {
        if (request.size != 4) {
            fail(STATUS_INVALID);
            return;
        }
        if (phase_ != Phase::Writing || offset_ != length_) {
            fail(STATUS_INVALID);
            return;
        }
        if (crc32(bytes_, length_) != checksum_) {
            reset();
            fail(STATUS_INVALID);
            return;
        }
        int error = decode_codeplug(bytes_, length_, plug_);
        if (!error && next_operation == UINT32_MAX) {
            error = -EOVERFLOW;
        }
        if (!error) {
            operation_id_ = ++next_operation;
            error = ui_replace_codeplug(plug_, operation_id_, radio_, baseline_.revision);
        }
        if (error) {
            reset();
            fail(result(error));
            return;
        }
        phase_ = Phase::Accepted;
        response.size = 5;
        sys_put_le32(token_, response.payload + 1);
        break;
    }
    case MSG_CPS_STATUS: {
        if (request.size != 4) {
            fail(STATUS_INVALID);
            return;
        }
        if (phase_ != Phase::Accepted) {
            fail(STATUS_STALE);
            return;
        }
        const auto status = settings_replacement_status();
        if (status.id != operation_id_) {
            fail(STATUS_STALE);
            return;
        }
        response.size = 16;
        sys_put_le32(token_, response.payload + 1);
        response.payload[5] = status.pending   ? CPS_STATE_ACCEPTED
                              : status.error   ? CPS_STATE_FAILED
                              : status.durable ? CPS_STATE_DURABLE
                                               : CPS_STATE_APPLIED;
        sys_put_le32(status.revision, response.payload + 6);
        sys_put_le32(status.generation, response.payload + 10);
        response.payload[14] = result(status.error);
        response.payload[15] = result(status.save_error);
        break;
    }
    case MSG_CPS_CANCEL:
        if (request.size != 4) {
            fail(STATUS_INVALID);
            return;
        }
        if (phase_ == Phase::Accepted) {
            fail(STATUS_BUSY);
            return;
        }
        reset();
        break;
    default:
        fail(STATUS_UNSUPPORTED);
        break;
    }
}
} // namespace companion
} // namespace ht
