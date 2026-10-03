// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <ht/m17_settings.hpp>
#include <stddef.h>
#include <stdint.h>

namespace ht {
namespace m17 {
struct Address {
    uint8_t bytes[6] = {};
};

struct LinkSetup {
    // Wire order: destination, source, type, metadata, CRC. No packed fields.
    uint8_t bytes[30] = {};
};

struct Payload {
    uint8_t bytes[16] = {};
};

struct Frame {
    uint8_t bytes[48] = {};
};
enum class AddressKind : uint8_t { Invalid, Station, Broadcast };

// Local calls: 1..9 uppercase letters, digits, '-', '/', '.'; no ALL/INVALID.
// On failure output is cleared. Input need not be null terminated.
bool encode_callsign(const char *text, size_t length, Address &output);
AddressKind decode_callsign(const Address &address, char (&output)[10]);
bool make_voice_link(const char *source, size_t length, LinkSetup &output,
                     const M17Settings &settings = {});
uint8_t channel_access_number(const LinkSetup &link);
// Accept only validated Codec2 voice addressed to broadcast/local, optionally
// matching CAN. Clears source on rejection; settings destination is TX-only.
bool accept_voice_link(const LinkSetup &link, const char (&local)[10], const M17Settings &settings,
                       char (&source)[10]);
bool valid_link(const LinkSetup &link);
// Unencrypted, unsigned Codec2 3200 voice. CAN and metadata are not filtered.
bool voice_stream(const LinkSetup &link);
void preamble(Frame &output);
void end_marker(Frame &output);

// All contexts and buffers are caller-owned, with no allocations or callbacks.
// One processing context owns each encoder/decoder. Start resets counters;
// stream before start or after the last frame fails and clears output.
class Encoder {
  public:
    void reset();
    bool start(const LinkSetup &link, Frame &output);
    bool stream(const Payload &payload, bool last, Frame &output);

  private:
    LinkSetup link_;
    uint16_t number_ = 0;
    uint8_t chunk_ = 0;
    bool ready_ = false;
};

enum class FrameKind : uint8_t { Unknown, Preamble, LinkSetup, Stream, End, Unsupported };

struct DecodedFrame {
    FrameKind kind = FrameKind::Unknown;
    bool link_updated = false; // Only a CRC-validated new LSF, direct or from LICH.
    bool payload_valid = false;
    bool last = false;
    uint16_t number = 0; // 15-bit frame number, without the last-frame bit.
    uint16_t errors = 0; // Hard-decision Viterbi distance on received coded bits.
    LinkSetup link;
    Payload payload; // Cleared on every decode, including a rejected frame.
};

class Decoder {
  public:
    void reset(); // Required at the start of a new receive session.
    DecodedFrame decode(const Frame &frame);

  private:
    LinkSetup partial_;
    uint8_t chunks_ = 0;
};
} // namespace m17
} // namespace ht
