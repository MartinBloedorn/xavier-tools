// Minimal OSC 1.0 encoding and UDP transmission.
//
// Only what this tool sends is implemented: no bundles, no pattern matching,
// no receiving. Types are limited to int32, int64 and float32, which covers
// sample counts, millisecond timestamps and band powers respectively.

#ifndef XAVIER_EPOC_OSC_HPP
#define XAVIER_EPOC_OSC_HPP

#include "epoc/epoc.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace epoc::osc {

/// A single OSC message: an address pattern, a type tag string, and arguments.
///
/// Reusable -- call reset() to start a new message without reallocating, which
/// matters a little when emitting ~1800 messages a second.
class Message {
public:
    Message() = default;
    explicit Message(std::string_view address) { reset(address); }

    /// Start a new message with this address, discarding any arguments.
    void reset(std::string_view address);

    Message& add(std::int32_t v);
    Message& add(std::int64_t v);
    Message& add(float v);
    Message& add(double v);
    /// Append an OSC time tag ('t'): 64-bit NTP, seconds since 1900 in the
    /// high word and a 2^-32 second fraction in the low word.
    Message& add_timetag(std::uint64_t ntp);

    /// The encoded packet. Always a multiple of 4 bytes, as OSC requires.
    const std::vector<std::uint8_t>& packet() const;

private:
    std::string address_;
    std::string tags_ = ",";
    std::vector<std::uint8_t> args_;
    mutable std::vector<std::uint8_t> packet_;
    mutable bool dirty_ = true;
};

/// Connectionless UDP sender. Resolves the destination once, at construction.
class UdpSender {
public:
    /// Throws Error if the host cannot be resolved or the socket cannot be
    /// created. Note that UDP has no connection, so a successful construction
    /// says nothing about whether anything is listening.
    UdpSender(const std::string& host, std::uint16_t port);
    ~UdpSender();

    UdpSender(UdpSender&&) noexcept;
    UdpSender& operator=(UdpSender&&) noexcept;
    UdpSender(const UdpSender&) = delete;
    UdpSender& operator=(const UdpSender&) = delete;

    /// Best-effort send. Datagram loss is counted, not thrown: a dropped
    /// packet must not take down a live acquisition.
    void send(const Message& m) noexcept;

    /// "host:port" as given, for display.
    const std::string& endpoint() const noexcept;
    /// The numeric address actually resolved to, e.g. "127.0.0.1". Worth
    /// showing: "localhost" can resolve to ::1, and a receiver bound only to
    /// IPv4 will then sit in silence with nothing to diagnose.
    const std::string& resolved_address() const noexcept;
    unsigned long long sent() const noexcept;
    unsigned long long failed() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// How to encode the timestamp argument.
///
/// This is selectable because OSC 1.0 only *requires* receivers to support
/// int32, float32, string and blob. int64, double and time tag are all
/// optional extensions, so which one a given receiver renders correctly is a
/// property of that receiver, not of the protocol. Hexler's Protokol, for
/// instance, displays an int64 through a 32-bit path and shows the low half,
/// which makes every current epoch-millisecond value look negative.
enum class TimestampFormat {
    Int64,        ///< 'h', UNIX epoch ms. Exact; the most precise option.
    Double,       ///< 'd', UNIX epoch ms. Exact (53-bit mantissa covers it).
    Int32Relative,///< 'i', ms since the stream began. Always supported.
    Timetag,      ///< 't', NTP time tag. The canonical OSC time type.
};

/// Parse "int64", "double", "int32" or "timetag". Returns false on anything
/// else, filling `error`.
bool parse_timestamp_format(std::string_view text, TimestampFormat& out, std::string& error);

/// Canonical name of a timestamp format, for display.
std::string_view timestamp_format_name(TimestampFormat f) noexcept;

/// Convert UNIX epoch milliseconds to a 64-bit NTP time tag.
std::uint64_t unix_ms_to_timetag(std::int64_t unix_ms) noexcept;

/// Which messages to emit. See parse_message_flags().
struct MessageFlags {
    bool timestamp = false;   ///< 't': prepend a timestamp argument
    bool bands = false;       ///< 'b': <prefix>/fft/<channel>
    bool raw = false;         ///< 'r': <prefix>/raw/<channel>
    bool raw_bundle = false;  ///< 'u': <prefix>/raw/all, every channel at once
    bool gyro = false;        ///< 'g': <prefix>/gyro/x and <prefix>/gyro/y
    /// 'y': <prefix>/battery, charge normalised to 0..1. Lettered 'y'
    /// because 'b' is already band powers.
    bool battery = false;
    /// 'q': contact quality. A modifier rather than a message in its own
    /// right -- it appends a quality argument to each <prefix>/raw/<channel>
    /// message and, alongside 'u', emits <prefix>/quality/all. On its own it
    /// sends nothing.
    bool quality = false;
};

/// Parse a flag string such as "tbr" or "tbru". Returns false and fills
/// `error` on an unknown or repeated character.
bool parse_message_flags(std::string_view spec, MessageFlags& out, std::string& error);

/// Render flags back as a canonical string, for display.
std::string format_message_flags(const MessageFlags& flags);

/// Normalise a user-supplied address prefix: guarantee exactly one leading
/// '/', drop any trailing '/', and collapse repeated slashes. An empty or
/// all-slash prefix yields "/".
std::string normalize_prefix(std::string_view prefix);

/// Parse "host:port", "[ipv6]:port", or a bare "port" (implying localhost).
/// Returns false and fills `error` if the string is unusable.
bool parse_endpoint(std::string_view text, std::string& host, std::uint16_t& port,
                    std::string& error);

/// Lowercased channel label, as used in OSC addresses ("af3", not "AF3").
/// OSC address patterns are case-sensitive, so this is fixed rather than
/// following the display casing.
std::string address_channel(Channel c);

/// A UNIX-epoch millisecond clock that cannot go backwards.
///
/// std::chrono::system_clock is the natural source for a UNIX timestamp but is
/// subject to NTP steps and manual clock changes, which would make a recording
/// jump or repeat timestamps. This anchors to the wall clock once and then
/// advances by a steady_clock delta, so the values are UNIX-epoch milliseconds
/// while remaining strictly monotonic within a run.
class MonotonicUnixClock {
public:
    MonotonicUnixClock();
    std::int64_t now_ms() const;

private:
    std::int64_t base_unix_ms_;
    std::int64_t base_steady_ns_;
};

}  // namespace epoc::osc

#endif  // XAVIER_EPOC_OSC_HPP
