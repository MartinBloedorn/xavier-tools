#include "epoc/osc.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>

#ifdef _WIN32
// winsock2.h must precede windows.h, or the older winsock.h gets pulled in
// first and the two conflict.
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
using socket_t = SOCKET;
static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <unistd.h>
using socket_t = int;
static constexpr socket_t kInvalidSocket = -1;
#endif

namespace epoc::osc {
namespace {

/// Append an OSC string: the bytes, then at least one NUL, then NUL padding
/// out to a multiple of four.
void append_osc_string(std::vector<std::uint8_t>& out, std::string_view s) {
    out.insert(out.end(), s.begin(), s.end());
    // Always at least one terminator, then round the total up to 4 bytes.
    const std::size_t padded = (s.size() / 4 + 1) * 4;
    out.insert(out.end(), padded - s.size(), 0);
}

/// OSC numbers are big-endian regardless of host byte order.
void append_be32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xff));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xff));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>(v & 0xff));
}

void append_be64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>((v >> shift) & 0xff));
    }
}

#ifdef _WIN32
/// Winsock needs explicit startup/shutdown, and is not reference counted.
class WinsockLibrary {
public:
    static void acquire() {
        std::lock_guard<std::mutex> lock(mutex());
        if (refcount()++ == 0) {
            WSADATA data;
            if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
                refcount()--;
                throw Error("WSAStartup() failed: could not initialise Windows sockets");
            }
        }
    }
    static void release() noexcept {
        std::lock_guard<std::mutex> lock(mutex());
        if (refcount() > 0 && --refcount() == 0) WSACleanup();
    }

private:
    static std::mutex& mutex() {
        static std::mutex m;
        return m;
    }
    static int& refcount() {
        static int n = 0;
        return n;
    }
};
#endif

void close_socket(socket_t s) noexcept {
    if (s == kInvalidSocket) return;
#ifdef _WIN32
    closesocket(s);
#else
    ::close(s);
#endif
}

}  // namespace

// ---------------------------------------------------------------------------
// Message
// ---------------------------------------------------------------------------

void Message::reset(std::string_view address) {
    address_.assign(address);
    tags_ = ",";
    args_.clear();
    dirty_ = true;
}

Message& Message::add(std::int32_t v) {
    tags_ += 'i';
    append_be32(args_, static_cast<std::uint32_t>(v));
    dirty_ = true;
    return *this;
}

Message& Message::add(std::int64_t v) {
    tags_ += 'h';
    append_be64(args_, static_cast<std::uint64_t>(v));
    dirty_ = true;
    return *this;
}

Message& Message::add(float v) {
    // Reinterpret the IEEE-754 bits; OSC float32 is the raw pattern, so a
    // numeric conversion would be wrong.
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(v), "float must be 32 bits");
    std::memcpy(&bits, &v, sizeof(bits));
    tags_ += 'f';
    append_be32(args_, bits);
    dirty_ = true;
    return *this;
}

Message& Message::add(double v) {
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(v), "double must be 64 bits");
    std::memcpy(&bits, &v, sizeof(bits));
    tags_ += 'd';
    append_be64(args_, bits);
    dirty_ = true;
    return *this;
}

Message& Message::add_timetag(std::uint64_t ntp) {
    tags_ += 't';
    append_be64(args_, ntp);
    dirty_ = true;
    return *this;
}

const std::vector<std::uint8_t>& Message::packet() const {
    if (dirty_) {
        packet_.clear();
        append_osc_string(packet_, address_);
        append_osc_string(packet_, tags_);
        packet_.insert(packet_.end(), args_.begin(), args_.end());
        dirty_ = false;
    }
    return packet_;
}

// ---------------------------------------------------------------------------
// UdpSender
// ---------------------------------------------------------------------------

struct UdpSender::Impl {
    socket_t fd = kInvalidSocket;
    sockaddr_storage addr{};
    socklen_t addr_len = 0;
    std::string endpoint;
    std::string resolved;
    unsigned long long sent = 0;
    unsigned long long failed = 0;

    ~Impl() {
        close_socket(fd);
#ifdef _WIN32
        if (winsock_held) WinsockLibrary::release();
#endif
    }
#ifdef _WIN32
    bool winsock_held = false;
#endif
};

UdpSender::UdpSender(const std::string& host, std::uint16_t port)
    : impl_(std::make_unique<Impl>()) {
#ifdef _WIN32
    WinsockLibrary::acquire();
    impl_->winsock_held = true;
#endif

    const std::string port_text = std::to_string(port);

    // Resolve IPv4 first, then fall back to anything.
    //
    // "localhost" resolves to ::1 before 127.0.0.1 on a dual-stack Windows
    // machine. Most OSC receivers bind IPv4 only, so preferring AF_UNSPEC
    // means packets vanish into the v6 loopback with nothing to diagnose --
    // UDP reports no error for a datagram nobody receives. Anyone who
    // genuinely wants IPv6 can write it as a bracketed literal.
    addrinfo hints{};
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    addrinfo* results = nullptr;
    hints.ai_family = AF_INET;
    int rc = getaddrinfo(host.c_str(), port_text.c_str(), &hints, &results);
    if (rc != 0 || !results) {
        hints.ai_family = AF_UNSPEC;
        rc = getaddrinfo(host.c_str(), port_text.c_str(), &hints, &results);
    }
    if (rc != 0 || !results) {
        throw Error("could not resolve OSC destination '" + host + ":" + port_text +
                    "'; check the host name or address");
    }

    impl_->fd = socket(results->ai_family, results->ai_socktype, results->ai_protocol);
    if (impl_->fd == kInvalidSocket) {
        freeaddrinfo(results);
        throw Error("could not create a UDP socket for OSC output");
    }

    std::memcpy(&impl_->addr, results->ai_addr, results->ai_addrlen);
    impl_->addr_len = static_cast<socklen_t>(results->ai_addrlen);

    // Record the numeric address so the caller can show which family we
    // actually picked.
    char numeric[NI_MAXHOST] = {};
    if (getnameinfo(results->ai_addr, static_cast<socklen_t>(results->ai_addrlen), numeric,
                    sizeof(numeric), nullptr, 0, NI_NUMERICHOST) == 0) {
        impl_->resolved = numeric;
    } else {
        impl_->resolved = host;
    }
    freeaddrinfo(results);

    impl_->endpoint = host + ":" + port_text;
}

UdpSender::~UdpSender() = default;
UdpSender::UdpSender(UdpSender&&) noexcept = default;
UdpSender& UdpSender::operator=(UdpSender&&) noexcept = default;

void UdpSender::send(const Message& m) noexcept {
    const auto& bytes = m.packet();
    if (bytes.empty() || impl_->fd == kInvalidSocket) return;

    const auto n = sendto(impl_->fd,
                          reinterpret_cast<const char*>(bytes.data()),
#ifdef _WIN32
                          static_cast<int>(bytes.size()),
#else
                          bytes.size(),
#endif
                          0, reinterpret_cast<const sockaddr*>(&impl_->addr),
                          impl_->addr_len);
    if (n < 0 || static_cast<std::size_t>(n) != bytes.size()) {
        ++impl_->failed;
    } else {
        ++impl_->sent;
    }
}

const std::string& UdpSender::endpoint() const noexcept { return impl_->endpoint; }
const std::string& UdpSender::resolved_address() const noexcept { return impl_->resolved; }
unsigned long long UdpSender::sent() const noexcept { return impl_->sent; }
unsigned long long UdpSender::failed() const noexcept { return impl_->failed; }

// ---------------------------------------------------------------------------
// Option parsing helpers
// ---------------------------------------------------------------------------

bool parse_message_flags(std::string_view spec, MessageFlags& out, std::string& error) {
    MessageFlags flags;
    for (const char c : spec) {
        bool* target = nullptr;
        switch (c) {
            case 't': target = &flags.timestamp; break;
            case 'b': target = &flags.bands; break;
            case 'r': target = &flags.raw; break;
            case 'u': target = &flags.raw_bundle; break;
            case 'g': target = &flags.gyro; break;
            case 'y': target = &flags.battery; break;
            case 'q': target = &flags.quality; break;
            default:
                error = std::string("unknown OSC message flag '") + c +
                        "'; valid flags are t (timestamp), b (bands), "
                        "r (raw per channel), u (all raw in one message), "
                        "g (gyro), y (battery), q (contact quality)";
                return false;
        }
        if (*target) {
            error = std::string("OSC message flag '") + c + "' given more than once";
            return false;
        }
        *target = true;
    }
    out = flags;
    return true;
}

std::string format_message_flags(const MessageFlags& flags) {
    std::string s;
    if (flags.timestamp) s += 't';
    if (flags.bands) s += 'b';
    if (flags.raw) s += 'r';
    if (flags.raw_bundle) s += 'u';
    if (flags.gyro) s += 'g';
    if (flags.battery) s += 'y';
    if (flags.quality) s += 'q';
    return s.empty() ? std::string("(none)") : s;
}

std::string normalize_prefix(std::string_view prefix) {
    std::string out;
    out.reserve(prefix.size() + 1);
    out.push_back('/');
    bool last_was_slash = true;
    for (const char c : prefix) {
        if (c == '/') {
            if (last_was_slash) continue;  // collapse runs of slashes
            last_was_slash = true;
            out.push_back('/');
        } else {
            last_was_slash = false;
            out.push_back(c);
        }
    }
    // Drop a trailing slash, but never reduce below the root "/".
    if (out.size() > 1 && out.back() == '/') out.pop_back();
    return out;
}

bool parse_endpoint(std::string_view text, std::string& host, std::uint16_t& port,
                    std::string& error) {
    if (text.empty()) {
        error = "empty OSC destination; expected host:port";
        return false;
    }

    std::string host_part;
    std::string port_part;

    if (text.front() == '[') {
        // Bracketed IPv6 literal, e.g. [::1]:9000
        const auto close = text.find(']');
        if (close == std::string_view::npos) {
            error = "unterminated IPv6 address in OSC destination '" + std::string(text) + "'";
            return false;
        }
        host_part = std::string(text.substr(1, close - 1));
        const auto rest = text.substr(close + 1);
        if (rest.empty() || rest.front() != ':') {
            error = "missing port in OSC destination '" + std::string(text) + "'";
            return false;
        }
        port_part = std::string(rest.substr(1));
    } else {
        const auto colon = text.rfind(':');
        if (colon == std::string_view::npos) {
            // A bare number is a port on the local machine, which is the
            // overwhelmingly common case for OSC.
            host_part = "127.0.0.1";
            port_part = std::string(text);
        } else {
            host_part = std::string(text.substr(0, colon));
            port_part = std::string(text.substr(colon + 1));
            if (host_part.empty()) host_part = "127.0.0.1";
        }
    }

    if (port_part.empty() ||
        !std::all_of(port_part.begin(), port_part.end(),
                     [](unsigned char c) { return std::isdigit(c) != 0; })) {
        error = "OSC port must be a number, got '" + port_part + "'";
        return false;
    }
    const unsigned long value = std::strtoul(port_part.c_str(), nullptr, 10);
    if (value == 0 || value > 65535) {
        error = "OSC port must be between 1 and 65535, got '" + port_part + "'";
        return false;
    }
    if (host_part.empty()) {
        error = "empty host in OSC destination '" + std::string(text) + "'";
        return false;
    }

    host = host_part;
    port = static_cast<std::uint16_t>(value);
    return true;
}

bool parse_timestamp_format(std::string_view text, TimestampFormat& out, std::string& error) {
    if (text == "int64" || text == "h") {
        out = TimestampFormat::Int64;
    } else if (text == "double" || text == "d") {
        out = TimestampFormat::Double;
    } else if (text == "int32" || text == "i") {
        out = TimestampFormat::Int32Relative;
    } else if (text == "timetag" || text == "t") {
        out = TimestampFormat::Timetag;
    } else {
        error = "unknown OSC timestamp format '" + std::string(text) +
                "'; expected int64, double, int32 or timetag";
        return false;
    }
    return true;
}

std::string_view timestamp_format_name(TimestampFormat f) noexcept {
    switch (f) {
        case TimestampFormat::Int64:         return "int64";
        case TimestampFormat::Double:        return "double";
        case TimestampFormat::Int32Relative: return "int32";
        case TimestampFormat::Timetag:       return "timetag";
    }
    return "?";
}

std::uint64_t unix_ms_to_timetag(std::int64_t unix_ms) noexcept {
    // NTP counts from 1900-01-01; UNIX from 1970-01-01. The offset is the
    // 70 years between them, including 17 leap days.
    constexpr std::int64_t kNtpUnixOffsetSeconds = 2208988800LL;
    if (unix_ms < 0) return 0;

    const std::int64_t seconds = unix_ms / 1000;
    const std::int64_t millis = unix_ms % 1000;
    const std::uint64_t ntp_seconds =
        static_cast<std::uint64_t>(seconds + kNtpUnixOffsetSeconds);
    // The low word is a fraction of a second in units of 2^-32.
    const std::uint64_t fraction =
        (static_cast<std::uint64_t>(millis) << 32) / 1000ULL;
    return (ntp_seconds << 32) | (fraction & 0xffffffffULL);
}

std::string address_channel(Channel c) {
    std::string s(channel_name(c));
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return s;
}

// ---------------------------------------------------------------------------
// MonotonicUnixClock
// ---------------------------------------------------------------------------

MonotonicUnixClock::MonotonicUnixClock() {
    base_unix_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    base_steady_ns_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
}

std::int64_t MonotonicUnixClock::now_ms() const {
    const std::int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count();
    return base_unix_ms_ + (now_ns - base_steady_ns_) / 1000000;
}

}  // namespace epoc::osc
