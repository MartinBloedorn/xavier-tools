// EPOC wire-protocol decoding: bit unpacking, key derivation, battery and
// contact-quality tables. All of this is a faithful port of emokit's
// src/emokit.c; the bit masks and lookup tables are reproduced verbatim.

#include "epoc/epoc.hpp"

#include <cstring>

namespace epoc {
namespace {

// Per-channel bit positions within the 32-byte report, indexed by Channel.
// Verbatim from emokit. Each entry names 14 packet bit positions, ordered
// least-significant first: element i supplies bit i of the 14-bit sample.
constexpr std::uint8_t kChannelMasks[kChannelCount][kChannelCount] = {
    /* AF3 */ {46, 47, 32, 33, 34, 35, 36, 37, 38, 39, 24, 25, 26, 27},
    /* F7  */ {48, 49, 50, 51, 52, 53, 54, 55, 40, 41, 42, 43, 44, 45},
    /* F3  */ {10, 11, 12, 13, 14, 15, 0, 1, 2, 3, 4, 5, 6, 7},
    /* FC5 */ {28, 29, 30, 31, 16, 17, 18, 19, 20, 21, 22, 23, 8, 9},
    /* T7  */ {66, 67, 68, 69, 70, 71, 56, 57, 58, 59, 60, 61, 62, 63},
    /* P7  */ {84, 85, 86, 87, 72, 73, 74, 75, 76, 77, 78, 79, 64, 65},
    /* O1  */ {102, 103, 88, 89, 90, 91, 92, 93, 94, 95, 80, 81, 82, 83},
    /* O2  */ {140, 141, 142, 143, 128, 129, 130, 131, 132, 133, 134, 135, 120, 121},
    /* P8  */ {158, 159, 144, 145, 146, 147, 148, 149, 150, 151, 136, 137, 138, 139},
    /* T8  */ {160, 161, 162, 163, 164, 165, 166, 167, 152, 153, 154, 155, 156, 157},
    /* FC6 */ {214, 215, 200, 201, 202, 203, 204, 205, 206, 207, 192, 193, 194, 195},
    /* F4  */ {216, 217, 218, 219, 220, 221, 222, 223, 208, 209, 210, 211, 212, 213},
    /* F8  */ {178, 179, 180, 181, 182, 183, 168, 169, 170, 171, 172, 173, 174, 175},
    /* AF4 */ {196, 197, 198, 199, 184, 185, 186, 187, 188, 189, 190, 191, 176, 177},
};

// The contact-quality reading shares one bit field; which electrode it refers
// to is selected by the report's first byte.
constexpr std::uint8_t kQualityMask[kChannelCount] = {
    99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112};

constexpr std::string_view kChannelNames[kChannelCount] = {
    "AF3", "F7", "F3", "FC5", "T7", "P7", "O1",
    "O2", "P8", "T8", "FC6", "F4", "F8", "AF4"};

/// Which electrode a quality reading describes, keyed by report byte 0.
/// Reproduced from emokit's handle_quality(). The table is odd -- F8, AF4 and
/// FC6 each appear more than once (selectors 10/14, 11/15, 12/80) and the
/// 64..80 block duplicates 0..16 -- but it is reproduced as-is rather than
/// "corrected", since this is the behaviour known to work against hardware.
bool quality_channel(std::uint8_t selector, Channel& out) noexcept {
    // The 64..80 block mirrors 0..16.
    const int s = (selector >= 64 && selector <= 80) ? (selector - 64) : selector;
    switch (s) {
        case 0:  out = Channel::F3;  return true;
        case 1:  out = Channel::FC5; return true;
        case 2:  out = Channel::AF3; return true;
        case 3:  out = Channel::F7;  return true;
        case 4:  out = Channel::T7;  return true;
        case 5:  out = Channel::P7;  return true;
        case 6:  out = Channel::O1;  return true;
        case 7:  out = Channel::O2;  return true;
        case 8:  out = Channel::P8;  return true;
        case 9:  out = Channel::T8;  return true;
        case 10: out = Channel::F8;  return true;
        case 11: out = Channel::AF4; return true;
        case 12: out = Channel::FC6; return true;
        case 13: out = Channel::F4;  return true;
        case 14: out = Channel::F8;  return true;
        case 15: out = Channel::AF4; return true;
        case 16: out = Channel::FC6; return true;  // reached via selector 80
        default: return false;
    }
}

}  // namespace

std::string_view channel_name(Channel c) noexcept {
    const auto i = static_cast<std::size_t>(c);
    return i < kChannelCount ? kChannelNames[i] : std::string_view{"??"};
}

std::string_view variant_name(DeviceVariant v) noexcept {
    return v == DeviceVariant::Research ? "research" : "consumer";
}

int unpack_level(const std::uint8_t packet[kPacketSize],
                 const std::uint8_t bits[kChannelCount]) noexcept {
    int level = 0;
    for (int i = static_cast<int>(kChannelCount) - 1; i >= 0; --i) {
        level <<= 1;
        const std::size_t byte = static_cast<std::size_t>(bits[i] / 8) + 1;
        const int bit = bits[i] % 8;
        level |= (packet[byte] >> bit) & 1;
    }
    return level;
}

std::uint8_t battery_percent(std::uint8_t raw) noexcept {
    // Emotiv reports battery on a coarse, non-linear scale. Table verbatim
    // from emokit's battery_value().
    if (raw >= 248) return 100;
    switch (raw) {
        case 247: return 99;
        case 246: return 97;
        case 245: return 93;
        case 244: return 89;
        case 243: return 85;
        case 242: return 82;
        case 241: return 77;
        case 240: return 72;
        case 239: return 66;
        case 238: return 62;
        case 237: return 55;
        case 236: return 46;
        case 235: return 32;
        case 234: return 20;
        case 233: return 12;
        case 232: return 6;
        case 231: return 4;
        case 230: return 3;
        case 229: return 2;
        case 228:
        case 227:
        case 226: return 1;
        default:  return 0;
    }
}

std::array<std::uint8_t, 16> derive_key(std::string_view serial, DeviceVariant variant) {
    if (serial.size() < 4) {
        throw Error("dongle serial '" + std::string(serial) +
                    "' is too short to derive a key from (need at least 4 characters)");
    }

    // Only the last four characters participate. emokit indexed the serial at
    // a hardcoded offset of 16, which silently produced a wrong key for any
    // serial that was not exactly 16 characters; we index from the actual end.
    const std::size_t n = serial.size();
    const auto c = [&](std::size_t back) {
        return static_cast<std::uint8_t>(serial[n - back]);
    };

    std::array<std::uint8_t, 16> k{};
    if (variant == DeviceVariant::Consumer) {
        k = {c(1), 0x00, c(2), 'H',
             c(1), 0x00, c(2), 'T',
             c(3), 0x10, c(4), 'B',
             c(3), 0x00, c(4), 'P'};
    } else {
        k = {c(1), 0x00, c(2), 'T',
             c(3), 0x10, c(4), 'B',
             c(1), 0x00, c(2), 'H',
             c(3), 0x00, c(4), 'P'};
    }
    // NOTE: emokit's doc/emotiv_protocol.asciidoc describes a *different*
    // consumer layout, with the (n-1,n-2) and (n-3,n-4) pairs at offsets 4..10
    // swapped relative to the above. The layout implemented here is the one in
    // emokit's C and Python code, which is the path known to have worked
    // against hardware, so it is the default. If decryption yields garbage on
    // a consumer dongle, try --research (see README).
    return k;
}

void decode_packet(const std::uint8_t packet[kPacketSize], Frame& out) noexcept {
    std::memcpy(out.raw.data(), packet, kPacketSize);

    // Once a second the dongle overwrites the sequence counter with a battery
    // reading, flagged by the high bit.
    if (packet[0] & 0x80) {
        out.counter = 128;
        out.is_battery_frame = true;
        out.battery = battery_percent(packet[0]);
    } else {
        out.counter = packet[0];
        out.is_battery_frame = false;
        // out.battery is carried in from the last battery frame.
    }

    for (std::size_t i = 0; i < kChannelCount; ++i) {
        out.counts[i] = unpack_level(packet, kChannelMasks[i]);
    }

    // emokit used offsets 102/104 here while emokit's own Python port used
    // 106/105. Neither is documented; these are the C values. The gyro reports
    // relative motion, so a constant offset error only shifts the zero point.
    out.gyro_x = static_cast<int>(packet[29]) - 102;
    out.gyro_y = static_cast<int>(packet[30]) - 104;

    // Contact quality: one electrode per report, so this updates a sticky
    // per-channel value rather than the whole set.
    Channel q_channel{};
    if (quality_channel(packet[0], q_channel)) {
        out.quality[static_cast<std::size_t>(q_channel)] = unpack_level(packet, kQualityMask);
    }
}

}  // namespace epoc
