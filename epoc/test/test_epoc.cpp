// Hardware-free tests for the EPOC protocol layer.
//
// These matter more than usual for this port: the reference implementation was
// validated only against live hardware, and the EPOC is discontinued, so the
// decode path needs to be checkable without a headset on the bench.

#include "epoc/epoc.hpp"

#include "../src/aes128.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++g_failures;
}

std::string hex(const std::uint8_t* p, std::size_t n) {
    std::string s;
    char buf[4];
    for (std::size_t i = 0; i < n; ++i) {
        std::snprintf(buf, sizeof(buf), "%02x", p[i]);
        s += buf;
    }
    return s;
}

void test_aes() {
    std::printf("AES-128 ECB\n");
    // FIPS-197 appendix C.1 -- the authoritative vector for AES-128.
    check(epoc::detail::Aes128Ecb::self_test(), "FIPS-197 C.1 decrypt vector");

    // Decrypting a block twice must be stable, and in-place must match
    // out-of-place (device.cpp relies on neither aliasing badly).
    const std::array<std::uint8_t, 16> key = {
        0x32, 0x00, 0x31, 0x48, 0x39, 0x00, 0x38, 0x54,
        0x32, 0x10, 0x31, 0x42, 0x39, 0x00, 0x38, 0x50};
    const epoc::detail::Aes128Ecb c(key);
    std::uint8_t in[16];
    for (int i = 0; i < 16; ++i) in[i] = static_cast<std::uint8_t>(i * 7 + 1);

    std::uint8_t out_a[16], out_b[16];
    c.decrypt_block(in, out_a);
    c.decrypt_block(in, out_b);
    check(std::memcmp(out_a, out_b, 16) == 0, "decrypt is deterministic");

    std::uint8_t inplace[16];
    std::memcpy(inplace, in, 16);
    c.decrypt_block(inplace, inplace);
    check(std::memcmp(inplace, out_a, 16) == 0, "in-place decrypt matches out-of-place");
}

void test_key_derivation() {
    std::printf("key derivation\n");

    // The worked example from emokit's doc/emotiv_protocol.asciidoc: serial
    // SN20120526998912, consumer headset. NOTE: the expected value below is
    // the one produced by emokit's *code* (C and Python agree). The prose in
    // that same document specifies a different consumer layout; the
    // discrepancy is real and is documented in derive_key(). This test pins
    // the code-derived behaviour so a future change to it is deliberate.
    const auto k = epoc::derive_key("SN20120526998912", epoc::DeviceVariant::Consumer);
    check(hex(k.data(), k.size()) == "32003148320031543910384239003850",
          "consumer layout for SN20120526998912");

    // Research layout for the same serial does match the prose exactly.
    const auto r = epoc::derive_key("SN20120526998912", epoc::DeviceVariant::Research);
    check(hex(r.data(), r.size()) == "32003154391038423200314839003850",
          "research layout for SN20120526998912 (matches protocol doc)");

    // The two layouts must actually differ, or variant selection is pointless.
    check(k != r, "consumer and research keys differ");

    // Only the last four characters participate, so a different prefix with
    // the same tail must give the same key. This is what lets --serial work.
    const auto same_tail = epoc::derive_key("XX8912", epoc::DeviceVariant::Consumer);
    check(same_tail == k, "only the last four serial characters matter");

    // emokit indexed the serial at a hardcoded offset of 16. A serial of a
    // different length must still key off its real last four characters.
    const auto short_serial = epoc::derive_key("8912", epoc::DeviceVariant::Consumer);
    check(short_serial == k, "short serial keys off its actual tail, not offset 16");

    bool threw = false;
    try {
        epoc::derive_key("abc", epoc::DeviceVariant::Consumer);
    } catch (const epoc::Error&) {
        threw = true;
    }
    check(threw, "serial shorter than 4 characters is rejected");
}

void test_unpack_level() {
    std::printf("bit unpacking\n");

    std::uint8_t packet[epoc::kPacketSize] = {};
    // unpack_level reads byte (bit/8)+1, bit (bit%8). It walks the mask from
    // index 13 down to 0, shifting left each step, so mask[i] ends up at bit
    // position i of the result: mask[0] is the LSB and mask[13] the MSB.
    const std::uint8_t mask[epoc::kChannelCount] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};

    check(epoc::unpack_level(packet, mask) == 0, "all-zero packet unpacks to 0");

    // mask[0] == packet bit 0 -> byte 0+1 = 1, bit 0.
    std::memset(packet, 0, sizeof(packet));
    packet[1] = 1 << 0;
    check(epoc::unpack_level(packet, mask) == 1, "mask[0] is the LSB");

    // mask[13] == packet bit 13 -> byte 13/8+1 = 2, bit 5, landing at bit 13.
    std::memset(packet, 0, sizeof(packet));
    packet[2] = 1 << 5;
    check(epoc::unpack_level(packet, mask) == (1 << 13), "mask[13] is the MSB");

    // Byte 0 of the report is the counter/battery field and must never be
    // read as sample data -- hence the +1 on the byte index.
    std::memset(packet, 0, sizeof(packet));
    packet[0] = 0xff;
    check(epoc::unpack_level(packet, mask) == 0, "packet byte 0 is not sample data");

    // A saturated field must produce exactly 14 bits set.
    std::memset(packet, 0xff, sizeof(packet));
    check(epoc::unpack_level(packet, mask) == 0x3fff, "saturated field is 14 bits");
}

void test_battery() {
    std::printf("battery table\n");
    check(epoc::battery_percent(255) == 100, "0xff is full");
    check(epoc::battery_percent(248) == 100, "248 is full");
    check(epoc::battery_percent(247) == 99, "247 -> 99%");
    check(epoc::battery_percent(230) == 3, "230 -> 3%");
    check(epoc::battery_percent(226) == 1, "226 -> 1%");
    check(epoc::battery_percent(225) == 0, "225 -> 0%");
    check(epoc::battery_percent(0) == 0, "0 -> 0%");

    // The table must be monotonically non-decreasing over its defined range;
    // a transcription slip would most likely show up as an inversion.
    bool monotonic = true;
    for (int i = 226; i < 255; ++i) {
        if (epoc::battery_percent(static_cast<std::uint8_t>(i)) >
            epoc::battery_percent(static_cast<std::uint8_t>(i + 1))) {
            monotonic = false;
        }
    }
    check(monotonic, "battery table is monotonic in 226..255");
}

void test_decode_packet() {
    std::printf("packet decode\n");

    std::uint8_t packet[epoc::kPacketSize] = {};
    epoc::Frame f;

    // A normal frame: byte 0 is the sequence counter.
    packet[0] = 42;
    epoc::decode_packet(packet, f);
    check(f.counter == 42 && !f.is_battery_frame, "counter frame reports its counter");

    // Every channel must be populated and, for a zero packet, read zero.
    bool all_zero = true;
    for (const auto ch : epoc::kChannels) {
        if (f.count(ch) != 0) all_zero = false;
    }
    check(all_zero, "zero packet decodes to zero on all 14 channels");

    // Gyro offsets are applied, so a zero packet reads negative, not zero.
    check(f.gyro_x == -102 && f.gyro_y == -104, "gyro offsets are applied");

    // A battery frame has the high bit set in byte 0.
    packet[0] = 0xff;
    epoc::decode_packet(packet, f);
    check(f.counter == 128 && f.is_battery_frame, "battery frame is flagged");
    check(f.battery == 100, "battery frame sets the percentage");

    // Battery must then persist across subsequent counter frames, since the
    // dongle only reports it once a second.
    packet[0] = 7;
    epoc::decode_packet(packet, f);
    check(!f.is_battery_frame && f.battery == 100, "battery persists across frames");

    // Contact quality is sticky too: selector 2 addresses AF3.
    std::memset(packet, 0, sizeof(packet));
    packet[0] = 2;
    std::memset(packet + 13, 0xff, 3);  // light up the quality bit field
    epoc::decode_packet(packet, f);
    const int af3_quality = f.contact_quality(epoc::Channel::AF3);
    check(af3_quality != 0, "selector 2 updates AF3 contact quality");

    packet[0] = 3;  // now F7; AF3 must retain its value
    epoc::decode_packet(packet, f);
    check(f.contact_quality(epoc::Channel::AF3) == af3_quality,
          "quality for other channels is retained");

    // Selector 80 is the one that maps through the 64..80 mirror to FC6.
    std::memset(packet, 0, sizeof(packet));
    packet[0] = 80;
    std::memset(packet + 13, 0xff, 3);
    epoc::decode_packet(packet, f);
    check(f.contact_quality(epoc::Channel::FC6) != 0, "selector 80 maps to FC6");
}

void test_channel_names() {
    std::printf("channel table\n");
    check(epoc::kChannels.size() == epoc::kChannelCount, "14 channels enumerated");

    // Names must be unique; a duplicate would mean a mis-ordered table and
    // would silently mislabel data in every output mode.
    std::vector<std::string> names;
    for (const auto ch : epoc::kChannels) names.emplace_back(epoc::channel_name(ch));
    std::vector<std::string> sorted = names;
    std::sort(sorted.begin(), sorted.end());
    check(std::unique(sorted.begin(), sorted.end()) == sorted.end(),
          "all 14 channel names are distinct");

    // Each Channel enumerator must sit at its own index.
    bool indexed = true;
    for (std::size_t i = 0; i < epoc::kChannels.size(); ++i) {
        if (static_cast<std::size_t>(epoc::kChannels[i]) != i) indexed = false;
    }
    check(indexed, "kChannels is indexed by enumerator value");

    check(epoc::channel_name(epoc::Channel::AF3) == "AF3", "AF3 name");
    check(epoc::channel_name(epoc::Channel::FC6) == "FC6", "FC6 name");
}

}  // namespace

int main() {
    std::printf("epoc protocol tests\n\n");
    test_aes();
    test_key_derivation();
    test_unpack_level();
    test_battery();
    test_decode_packet();
    test_channel_names();

    std::printf("\n%s\n", g_failures == 0 ? "all tests passed"
                                          : (std::to_string(g_failures) + " test(s) FAILED").c_str());
    return g_failures == 0 ? 0 : 1;
}
