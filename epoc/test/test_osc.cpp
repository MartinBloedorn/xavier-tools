// Tests for OSC encoding and option parsing.
//
// The encoder is checked byte-for-byte against hand-computed packets. OSC's
// padding rules are the easy thing to get subtly wrong -- particularly the
// requirement that a string whose length is already a multiple of four still
// gets a full four bytes of padding -- and a receiver given a malformed packet
// typically just drops it silently, so this would otherwise fail invisibly.

#include "epoc/osc.hpp"

#include "check.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;

std::string hex(const Bytes& b) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    for (std::size_t i = 0; i < b.size(); ++i) {
        if (i && i % 4 == 0) s += ' ';
        s += digits[b[i] >> 4];
        s += digits[b[i] & 0xf];
    }
    return s;
}

void expect_bytes(const epoc::osc::Message& m, const Bytes& want, const std::string& what) {
    const auto& got = m.packet();
    if (got == want) {
        tst::check(true, what);
    } else {
        std::printf("  [FAIL] %s\n         got  %s\n         want %s\n", what.c_str(),
                    hex(got).c_str(), hex(want).c_str());
        ++tst::failures();
    }
}

void test_string_padding() {
    tst::section("OSC string padding");

    // "/a" is 2 bytes; +1 terminator = 3; padded to 4.
    // Type tag "," is 1 byte; +1 = 2; padded to 4.
    epoc::osc::Message m("/a");
    expect_bytes(m, Bytes{'/', 'a', 0, 0, ',', 0, 0, 0}, "short address pads to 4");

    // "/abc" is already a multiple of 4, so OSC still requires a NUL and
    // padding to the *next* multiple: 8 bytes, not 4.
    epoc::osc::Message m2("/abc");
    expect_bytes(m2, Bytes{'/', 'a', 'b', 'c', 0, 0, 0, 0, ',', 0, 0, 0},
                 "4-byte address pads out to 8, not 4");

    // Every packet must be a multiple of 4 bytes.
    bool all_aligned = true;
    for (const char* addr : {"/a", "/ab", "/abc", "/abcd", "/abcde", "/epoc/raw/af3"}) {
        epoc::osc::Message probe(addr);
        probe.add(std::int32_t{1});
        if (probe.packet().size() % 4 != 0) all_aligned = false;
    }
    tst::check(all_aligned, "every packet length is a multiple of 4");
}

void test_argument_encoding() {
    tst::section("argument encoding");

    // int32 is big-endian.
    epoc::osc::Message i("/t");
    i.add(std::int32_t{42});
    expect_bytes(i, Bytes{'/', 't', 0, 0, ',', 'i', 0, 0, 0, 0, 0, 42}, "int32 is big-endian");

    // Negative int32 keeps two's-complement bits.
    epoc::osc::Message neg("/t");
    neg.add(std::int32_t{-2});
    expect_bytes(neg, Bytes{'/', 't', 0, 0, ',', 'i', 0, 0, 0xff, 0xff, 0xff, 0xfe},
                 "negative int32 is two's complement, big-endian");

    // int64 ('h'), used for millisecond timestamps.
    epoc::osc::Message h("/t");
    h.add(std::int64_t{1});
    expect_bytes(h, Bytes{'/', 't', 0, 0, ',', 'h', 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
                 "int64 is big-endian");

    // A realistic epoch value must survive exactly -- this is the reason the
    // timestamp is int64 and not float32, which has only 24 bits of mantissa.
    const std::int64_t epoch_ms = 1758067200123LL;
    epoc::osc::Message ts("/t");
    ts.add(epoch_ms);
    const auto& p = ts.packet();
    std::int64_t round_trip = 0;
    for (std::size_t k = 0; k < 8; ++k) {
        round_trip = (round_trip << 8) | p[8 + k];
    }
    tst::check(round_trip == epoch_ms, "millisecond epoch timestamp round-trips exactly");

    // float32 carries the raw IEEE-754 bit pattern. 1.0f is 0x3f800000.
    epoc::osc::Message f("/t");
    f.add(1.0f);
    expect_bytes(f, Bytes{'/', 't', 0, 0, ',', 'f', 0, 0, 0x3f, 0x80, 0x00, 0x00},
                 "float32 is the IEEE-754 bit pattern, big-endian");

    // Type tags must appear in argument order.
    epoc::osc::Message mixed("/t");
    mixed.add(std::int64_t{0}).add(1.0f).add(std::int32_t{0});
    const auto& mp = mixed.packet();
    tst::check(mp[4] == ',' && mp[5] == 'h' && mp[6] == 'f' && mp[7] == 'i',
               "type tags follow argument order");
}

void test_timestamp_formats() {
    tst::section("timestamp formats");

    const std::int64_t epoch_ms = 1789673292861LL;  // a real observed value

    // int64: exact, and crucially NOT negative. A receiver that truncates
    // this to 32 bits sees the low word, which is negative for essentially
    // every current epoch-millisecond value -- the symptom that motivated
    // making the encoding selectable.
    epoc::osc::Message h("/t");
    h.add(epoch_ms);
    const auto& hp = h.packet();
    tst::check((hp[8] & 0x80) == 0, "int64 timestamp has a clear sign bit");
    std::int64_t back = 0;
    for (std::size_t k = 0; k < 8; ++k) back = (back << 8) | hp[8 + k];
    tst::check(back == epoch_ms, "int64 timestamp is exact");
    const std::int32_t low_word =
        static_cast<std::int32_t>(static_cast<std::uint32_t>(epoch_ms & 0xffffffffu));
    tst::check(low_word < 0,
               "low 32 bits really are negative (explains truncating receivers)");

    // double: a 53-bit mantissa represents a 41-bit millisecond epoch exactly.
    epoc::osc::Message d("/t");
    d.add(static_cast<double>(epoch_ms));
    const auto& dp = d.packet();
    tst::check(dp[5] == 'd', "double uses type tag 'd'");
    std::uint64_t dbits = 0;
    for (std::size_t k = 0; k < 8; ++k) dbits = (dbits << 8) | dp[8 + k];
    double dv = 0;
    std::memcpy(&dv, &dbits, sizeof(dv));
    tst::check(static_cast<std::int64_t>(dv) == epoch_ms,
               "double timestamp round-trips a millisecond epoch exactly");
    tst::check(dv > 0, "double timestamp is positive");

    // Confirm the claim behind the int32 option: float32 could NOT do this.
    const float as_float = static_cast<float>(epoch_ms);
    tst::check(static_cast<std::int64_t>(as_float) != epoch_ms,
               "float32 would lose precision (which is why it is not offered)");

    // timetag: NTP seconds sit in the high word, offset by 70 years.
    const std::uint64_t tag = epoc::osc::unix_ms_to_timetag(epoch_ms);
    const std::uint64_t ntp_seconds = tag >> 32;
    const std::uint64_t fraction = tag & 0xffffffffULL;
    tst::check(ntp_seconds == static_cast<std::uint64_t>(epoch_ms / 1000 + 2208988800LL),
               "timetag seconds carry the NTP epoch offset");
    // 861 ms should be ~0.861 of a second in 2^-32 units.
    const double frac_seconds = static_cast<double>(fraction) / 4294967296.0;
    tst::check_near(frac_seconds, 0.861, 0.001, "timetag fraction encodes the milliseconds");

    epoc::osc::Message t("/t");
    t.add_timetag(tag);
    tst::check(t.packet()[5] == 't', "timetag uses type tag 't'");

    // Format-name parsing, both long and short spellings.
    epoc::osc::TimestampFormat f;
    std::string err;
    tst::check(epoc::osc::parse_timestamp_format("int64", f, err) &&
                   f == epoc::osc::TimestampFormat::Int64, "'int64' parses");
    tst::check(epoc::osc::parse_timestamp_format("double", f, err) &&
                   f == epoc::osc::TimestampFormat::Double, "'double' parses");
    tst::check(epoc::osc::parse_timestamp_format("int32", f, err) &&
                   f == epoc::osc::TimestampFormat::Int32Relative, "'int32' parses");
    tst::check(epoc::osc::parse_timestamp_format("timetag", f, err) &&
                   f == epoc::osc::TimestampFormat::Timetag, "'timetag' parses");
    tst::check(!epoc::osc::parse_timestamp_format("float", f, err),
               "an unsupported format is rejected");
    tst::check(epoc::osc::timestamp_format_name(epoc::osc::TimestampFormat::Double) == "double",
               "format names round-trip");
}

void test_message_reuse() {
    tst::section("message reuse");

    epoc::osc::Message m("/one");
    m.add(std::int32_t{1});
    const Bytes first = m.packet();

    m.reset("/one");
    m.add(std::int32_t{1});
    tst::check(m.packet() == first, "reset then rebuild gives an identical packet");

    // "/two" is 4 chars -> 8 bytes once NUL-terminated and padded; the empty
    // type tag "," takes another 4. No argument bytes must remain.
    m.reset("/two");
    expect_bytes(m, Bytes{'/', 't', 'w', 'o', 0, 0, 0, 0, ',', 0, 0, 0},
                 "reset clears previous arguments");
}

void test_realistic_messages() {
    tst::section("realistic messages");

    // What the tool actually emits for a raw channel sample.
    epoc::osc::Message raw("/epoc/raw/af3");
    raw.add(std::int64_t{1758067200123LL}).add(std::int32_t{8412});
    const auto& p = raw.packet();
    // "/epoc/raw/af3" is 13 bytes -> padded to 16. ",hi" is 3 -> padded to 4.
    // Plus 8 bytes of int64 and 4 of int32 = 32 total.
    tst::check(p.size() == 32, "raw channel message is 32 bytes");
    tst::check(std::memcmp(p.data(), "/epoc/raw/af3", 13) == 0, "raw address is intact");
    tst::check(p[16] == ',' && p[17] == 'h' && p[18] == 'i', "raw type tags are ,hi");

    // A band message: timestamp plus five floats.
    epoc::osc::Message fft("/epoc/fft/af3");
    fft.add(std::int64_t{0});
    for (int k = 0; k < 5; ++k) fft.add(1.0f);
    // 16 (address) + 8 (",hfffff" is 7 -> 8) + 8 + 20 = 52.
    tst::check(fft.packet().size() == 52, "band message is 52 bytes");

    // With the quality flag, each raw message carries a second int32.
    epoc::osc::Message rawq("/epoc/raw/af3");
    rawq.add(std::int64_t{1758067200123LL}).add(std::int32_t{8412}).add(std::int32_t{6881});
    const auto& pq = rawq.packet();
    // 16 (address) + 8 (",hii" is 4 -> 8, a tag string needs its NUL) + 8 + 4
    // + 4 = 40.
    tst::check(pq.size() == 40, "raw channel message with quality is 40 bytes");
    tst::check(pq[16] == ',' && pq[17] == 'h' && pq[18] == 'i' && pq[19] == 'i',
               "raw-with-quality type tags are ,hii");

    // The bundled raw message carries all 14 channels.
    epoc::osc::Message all("/epoc/raw/all");
    all.add(std::int64_t{0});
    for (std::size_t k = 0; k < epoc::kChannelCount; ++k) all.add(std::int32_t{0});
    // 16 + (",h" + 14 'i' = 16 chars -> 20) + 8 + 56 = 100.
    tst::check(all.packet().size() == 100, "bundled raw message is 100 bytes");

    // /quality/all mirrors it: same shape, same channel order, four bytes
    // wider only because the address is longer.
    epoc::osc::Message qall("/epoc/quality/all");
    qall.add(std::int64_t{0});
    for (std::size_t k = 0; k < epoc::kChannelCount; ++k) qall.add(std::int32_t{0});
    // "/epoc/quality/all" is 17 -> 20, tags 16 -> 20, plus 8 + 56 = 104.
    tst::check(qall.packet().size() == 104, "bundled quality message is 104 bytes");
    tst::check(std::memcmp(qall.packet().data(), "/epoc/quality/all", 17) == 0,
               "quality address is intact");

    // Battery: a timestamp and one normalised float. The headset reports
    // whole percent, so 97% must land on exactly 0.97f, not near it.
    epoc::osc::Message batt("/epoc/battery");
    batt.add(std::int64_t{1758067200123LL}).add(97.0f / 100.0f);
    const auto& bp = batt.packet();
    // "/epoc/battery" is 13 -> 16, ",hf" is 3 -> 4, plus 8 and 4 = 32.
    tst::check(bp.size() == 32, "battery message is 32 bytes");
    tst::check(bp[16] == ',' && bp[17] == 'h' && bp[18] == 'f',
               "battery type tags are ,hf");
    std::uint32_t bits = 0;
    for (std::size_t k = 0; k < 4; ++k) bits = (bits << 8) | bp[28 + k];
    float level = 0.0f;
    std::memcpy(&level, &bits, sizeof(level));
    tst::check(level == 0.97f, "battery level encodes as 0.97");
    tst::check(level >= 0.0f && level <= 1.0f, "battery level is normalised to 0..1");
}

void test_message_flags() {
    tst::section("message flags");

    epoc::osc::MessageFlags f;
    std::string err;

    tst::check(epoc::osc::parse_message_flags("tbr", f, err), "'tbr' parses");
    tst::check(f.timestamp && f.bands && f.raw && !f.raw_bundle && !f.gyro &&
                   !f.battery && !f.quality,
               "'tbr' sets t, b, r only");

    // The shipped default.
    tst::check(epoc::osc::parse_message_flags("tbrgy", f, err),
               "'tbrgy' (the default) parses");
    tst::check(f.timestamp && f.bands && f.raw && f.gyro && f.battery &&
                   !f.raw_bundle && !f.quality,
               "'tbrgy' sets t, b, r, g, y but not u or q");

    // Battery is lettered 'y' because 'b' is band powers. Guard against the
    // obvious slip of wiring 'b' to both.
    tst::check(epoc::osc::parse_message_flags("b", f, err) && f.bands && !f.battery,
               "'b' is band powers, not battery");
    tst::check(epoc::osc::parse_message_flags("y", f, err) && f.battery && !f.bands,
               "'y' is battery, not band powers");
    tst::check(epoc::osc::parse_message_flags("q", f, err) && f.quality && !f.bands,
               "'q' is contact quality, not band powers");

    tst::check(epoc::osc::parse_message_flags("tbrugyq", f, err), "'tbrugyq' parses");
    tst::check(f.timestamp && f.bands && f.raw && f.raw_bundle && f.gyro &&
                   f.battery && f.quality,
               "'tbrugyq' sets all seven");

    // Order must not matter.
    epoc::osc::MessageFlags g;
    tst::check(epoc::osc::parse_message_flags("qygurbt", g, err), "'qygurbt' parses");
    tst::check(g.timestamp == f.timestamp && g.bands == f.bands && g.raw == f.raw &&
                   g.raw_bundle == f.raw_bundle && g.gyro == f.gyro &&
                   g.battery == f.battery && g.quality == f.quality,
               "flag order does not matter");

    tst::check(epoc::osc::parse_message_flags("", f, err), "empty flag string parses");
    tst::check(!f.timestamp && !f.bands && !f.raw && !f.raw_bundle && !f.gyro &&
                   !f.battery && !f.quality,
               "empty flag string enables nothing");

    tst::check(!epoc::osc::parse_message_flags("tbx", f, err), "unknown flag is rejected");
    tst::check(err.find('x') != std::string::npos, "error names the offending flag");

    tst::check(!epoc::osc::parse_message_flags("tt", f, err), "repeated flag is rejected");

    epoc::osc::MessageFlags all;
    all.timestamp = all.bands = all.raw = all.raw_bundle = all.gyro = all.battery =
        all.quality = true;
    tst::check(epoc::osc::format_message_flags(all) == "tbrugyq", "flags format canonically");

    // Round-trip: formatting then reparsing must preserve every flag.
    epoc::osc::MessageFlags again;
    tst::check(epoc::osc::parse_message_flags(epoc::osc::format_message_flags(all), again, err) &&
                   again.timestamp == all.timestamp && again.bands == all.bands &&
                   again.raw == all.raw && again.raw_bundle == all.raw_bundle &&
                   again.gyro == all.gyro && again.battery == all.battery &&
                   again.quality == all.quality,
               "format then parse round-trips");
}

void test_normalize_prefix() {
    tst::section("prefix normalisation");

    using epoc::osc::normalize_prefix;
    tst::check(normalize_prefix("/epoc") == "/epoc", "already-normal prefix is unchanged");
    tst::check(normalize_prefix("epoc") == "/epoc", "missing leading slash is added");
    tst::check(normalize_prefix("/epoc/") == "/epoc", "trailing slash is dropped");
    tst::check(normalize_prefix("//epoc//") == "/epoc", "repeated slashes collapse");
    tst::check(normalize_prefix("/a/b/c") == "/a/b/c", "nested prefixes survive");
    tst::check(normalize_prefix("") == "/", "empty prefix becomes root");
    tst::check(normalize_prefix("/") == "/", "root stays root");
    tst::check(normalize_prefix("///") == "/", "all-slash prefix becomes root");
}

void test_parse_endpoint() {
    tst::section("endpoint parsing");

    std::string host;
    std::uint16_t port = 0;
    std::string err;
    using epoc::osc::parse_endpoint;

    tst::check(parse_endpoint("127.0.0.1:9000", host, port, err) && host == "127.0.0.1" &&
                   port == 9000,
               "host:port parses");

    tst::check(parse_endpoint("localhost:7777", host, port, err) && host == "localhost" &&
                   port == 7777,
               "hostname:port parses");

    // A bare port is the common case: send to something on this machine.
    tst::check(parse_endpoint("9000", host, port, err) && host == "127.0.0.1" && port == 9000,
               "bare port implies localhost");

    tst::check(parse_endpoint("[::1]:9000", host, port, err) && host == "::1" && port == 9000,
               "bracketed IPv6 literal parses");

    // rfind(':') is what makes an unbracketed IPv6 address ambiguous, so the
    // brackets are required; check we do not silently mangle one.
    tst::check(parse_endpoint(":9000", host, port, err) && host == "127.0.0.1" && port == 9000,
               "leading colon implies localhost");

    tst::check(!parse_endpoint("", host, port, err), "empty destination is rejected");
    tst::check(!parse_endpoint("host:", host, port, err), "missing port is rejected");
    tst::check(!parse_endpoint("host:abc", host, port, err), "non-numeric port is rejected");
    tst::check(!parse_endpoint("host:0", host, port, err), "port 0 is rejected");
    tst::check(!parse_endpoint("host:70000", host, port, err), "out-of-range port is rejected");
    tst::check(!parse_endpoint("[::1", host, port, err), "unterminated IPv6 is rejected");
}

void test_address_channel() {
    tst::section("channel addresses");

    tst::check(epoc::osc::address_channel(epoc::Channel::AF3) == "af3", "AF3 -> af3");
    tst::check(epoc::osc::address_channel(epoc::Channel::FC6) == "fc6", "FC6 -> fc6");

    // Addresses must be unique and contain no OSC pattern metacharacters,
    // which would make them unroutable by a receiver.
    std::vector<std::string> seen;
    bool clean = true;
    for (const auto ch : epoc::kChannels) {
        const auto a = epoc::osc::address_channel(ch);
        if (a.find_first_of(" #*,/?[]{}") != std::string::npos) clean = false;
        for (const auto& prev : seen) {
            if (prev == a) clean = false;
        }
        seen.push_back(a);
    }
    tst::check(clean, "all 14 channel addresses are unique and pattern-safe");
    tst::check(seen.size() == epoc::kChannelCount, "every channel has an address");
}

void test_monotonic_clock() {
    tst::section("monotonic clock");

    epoc::osc::MonotonicUnixClock clock;
    const auto a = clock.now_ms();

    // Should be a plausible UNIX epoch in milliseconds: after 2020-01-01 and
    // before 2100. A seconds-vs-milliseconds mix-up fails this immediately.
    tst::check(a > 1577836800000LL, "timestamp is after 2020 (so it is in ms, not s)");
    tst::check(a < 4102444800000LL, "timestamp is before 2100");

    bool never_decreases = true;
    auto prev = a;
    for (int i = 0; i < 2000; ++i) {
        const auto now = clock.now_ms();
        if (now < prev) never_decreases = false;
        prev = now;
    }
    tst::check(never_decreases, "clock never goes backwards");
}

}  // namespace

int main() {
    std::printf("epoc osc tests\n\n");
    test_string_padding();
    test_argument_encoding();
    test_timestamp_formats();
    test_message_reuse();
    test_realistic_messages();
    test_message_flags();
    test_normalize_prefix();
    test_parse_endpoint();
    test_address_channel();
    test_monotonic_clock();
    return tst::summary();
}
