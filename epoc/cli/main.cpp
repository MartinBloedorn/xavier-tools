// xavier-tools `epoc`: read the Emotiv EPOC's 14 EEG channels in real time.

#include "epoc/dsp.hpp"
#include "epoc/epoc.hpp"
#include "epoc/osc.hpp"

#include "terminal.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr const char* kProgram = "epoc";
#ifndef XAVIER_EPOC_VERSION
#  define XAVIER_EPOC_VERSION "unknown"
#endif
constexpr const char* kVersion = XAVIER_EPOC_VERSION;

/// Contact quality full-scale for the bar display. emokit's header notes that
/// values above 4000 indicate a good contact; the units are otherwise
/// undocumented, so this is a display convention, not a calibrated threshold.
constexpr int kQualityGood = 4000;

/// Rendered width of the live table, in columns. Kept as a constant so the
/// narrow-terminal warning stays honest if the columns are ever changed.
constexpr int kTableWidth = 99;

/// How often the battery level is resent over OSC even when it has not
/// changed, so a receiver that joins mid-stream learns the charge without
/// waiting for it to move. Only ever tested against a battery frame, which
/// arrives about once a second, so the real gap is this rounded up to the
/// next such frame -- close enough for a value that moves in 13 steps.
constexpr auto kBatteryResend = std::chrono::seconds{5};

struct Options {
    bool list = false;
    bool help = false;
    bool version = false;
    bool show_key = false;
    /// Show band power as a percentage of each channel's total instead of
    /// absolute uV^2.
    bool relative = false;
    std::optional<std::size_t> index;
    std::optional<std::string> path;
    std::optional<std::string> serial;
    std::chrono::milliseconds refresh{50};
    /// How long to wait for a candidate interface to produce its first
    /// packet before moving on to the next one.
    std::chrono::milliseconds settle{2000};
    /// FFT window length in samples; must be a power of two.
    std::size_t window = 256;
    /// Keep listening when the dongle is present but the headset is silent,
    /// instead of failing. The headset sleeps on its own, so this is the
    /// friendlier default.
    bool wait = true;

    /// OSC destination ("host:port", or a bare port). Empty disables OSC.
    std::optional<std::string> osc_target;
    std::string osc_prefix = "/epoc";
    std::string osc_messages = "tbrgy";
    epoc::osc::TimestampFormat osc_timestamp = epoc::osc::TimestampFormat::Int64;
};

void print_usage(std::FILE* out) {
    std::fprintf(out,
        "%s %s -- read Emotiv EPOC EEG channels in real time\n"
        "\n"
        "usage: %s [options]\n"
        "\n"
        "options:\n"
        "  -l, --list            list connected Emotiv dongles and exit\n"
        "  -i, --index N         open the Nth dongle (default: probe automatically)\n"
        "      --path PATH       open a specific HID path (see --list)\n"
        "      --serial SN       override the serial used for key derivation\n"
        "      --show-key        print the derived AES key and exit (debugging)\n"
        "      --refresh MS      display refresh interval, ms (default: 50)\n"
        "      --settle MS       per-interface probe timeout, ms (default: 2000)\n"
        "      --no-wait         fail if the headset is not already streaming\n"
        "  -w, --window N        FFT window in samples, power of two (default: 256)\n"
        "  -r, --relative        show band power as %% of each channel's total\n"
        "      --osc DEST        stream OSC to host:port (or a bare port)\n"
        "      --osc-prefix P    OSC address prefix (default: /epoc)\n"
        "      --osc-messages F  which OSC messages to send (default: tbrgy)\n"
        "      --osc-timestamp T timestamp encoding: int64 (default), double,\n"
        "                        int32 or timetag\n"
        "  -h, --help            show this help and exit\n"
        "  -V, --version         show version and exit\n"
        "\n"
        "Band powers are computed per channel over a sliding window: 256 samples\n"
        "is 2 s at 128 Hz, giving 0.5 Hz resolution. A longer window resolves the\n"
        "low bands better but responds more slowly.\n"
        "\n"
        "OSC message flags, combined in any order:\n"
        "  t  prepend a UNIX millisecond timestamp to every message\n"
        "  r  per-channel raw samples:  <prefix>/raw/af3 <ts> <raw>\n"
        "  b  per-channel band powers:  <prefix>/fft/af3 <ts> <delta> ... <gamma>\n"
        "  u  all raw channels at once: <prefix>/raw/all <ts> <af3> <f7> ...\n"
        "  g  gyro axes:                <prefix>/gyro/x and <prefix>/gyro/y\n"
        "  y  battery charge, 0..1:     <prefix>/battery <ts> <level>\n"
        "  q  contact quality:          appends <quality> to each raw message;\n"
        "                               with u, also <prefix>/quality/all\n"
        "Raw and gyro messages are sent per sample (128 Hz); band messages are sent\n"
        "each time the spectrum is recomputed, i.e. once per --refresh interval.\n"
        "Battery is sent once the first reading arrives, then whenever the charge\n"
        "changes and every 5 s regardless, so a receiver that joins late still\n"
        "learns the level. It is a rare message rather than a stream.\n"
        "The dongle reports quality for one electrode per packet, so quality/all is\n"
        "sent about 34 times a second. q is not on by default: it changes how many\n"
        "arguments the raw messages carry.\n"
        "\n"
        "OSC 1.0 only requires receivers to support int32, float32, string and\n"
        "blob; int64, double and timetag are optional extensions. If your receiver\n"
        "shows negative or nonsensical timestamps it is likely truncating the\n"
        "int64 -- try --osc-timestamp double, or int32 for maximum portability.\n",
        kProgram, kVersion, kProgram);
}

bool parse_ms(const char* v, long lo, long hi, std::string_view flag,
              std::chrono::milliseconds& out) {
    const long ms = std::strtol(v, nullptr, 10);
    if (ms < lo || ms > hi) {
        std::fprintf(stderr, "%s: %.*s must be between %ld and %ld ms\n", kProgram,
                     static_cast<int>(flag.size()), flag.data(), lo, hi);
        return false;
    }
    out = std::chrono::milliseconds{ms};
    return true;
}

/// Returns false and prints a diagnostic if the arguments are unusable.
bool parse_args(int argc, char** argv, Options& opts) {
    const auto need_value = [&](int& i, std::string_view flag) -> const char* {
        if (i + 1 >= argc) {
            std::fprintf(stderr, "%s: %.*s requires a value\n", kProgram,
                         static_cast<int>(flag.size()), flag.data());
            return nullptr;
        }
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "-l" || a == "--list") {
            opts.list = true;
        } else if (a == "-h" || a == "--help") {
            opts.help = true;
        } else if (a == "-V" || a == "--version") {
            opts.version = true;
        } else if (a == "--show-key") {
            opts.show_key = true;
        } else if (a == "-r" || a == "--relative") {
            opts.relative = true;
        } else if (a == "--no-wait") {
            opts.wait = false;
        } else if (a == "--osc") {
            const char* v = need_value(i, a);
            if (!v) return false;
            opts.osc_target = v;
        } else if (a == "--osc-prefix") {
            const char* v = need_value(i, a);
            if (!v) return false;
            opts.osc_prefix = v;
        } else if (a == "--osc-messages") {
            const char* v = need_value(i, a);
            if (!v) return false;
            opts.osc_messages = v;
            // Validate now: a typo here should fail before we open hardware.
            epoc::osc::MessageFlags probe;
            std::string error;
            if (!epoc::osc::parse_message_flags(opts.osc_messages, probe, error)) {
                std::fprintf(stderr, "%s: %s\n", kProgram, error.c_str());
                return false;
            }
        } else if (a == "--osc-timestamp") {
            const char* v = need_value(i, a);
            if (!v) return false;
            std::string error;
            if (!epoc::osc::parse_timestamp_format(v, opts.osc_timestamp, error)) {
                std::fprintf(stderr, "%s: %s\n", kProgram, error.c_str());
                return false;
            }
        } else if (a == "-i" || a == "--index") {
            const char* v = need_value(i, a);
            if (!v) return false;
            opts.index = static_cast<std::size_t>(std::strtoul(v, nullptr, 10));
        } else if (a == "--path") {
            const char* v = need_value(i, a);
            if (!v) return false;
            opts.path = v;
        } else if (a == "--serial") {
            const char* v = need_value(i, a);
            if (!v) return false;
            opts.serial = v;
        } else if (a == "--refresh") {
            const char* v = need_value(i, a);
            if (!v || !parse_ms(v, 1, 10000, a, opts.refresh)) return false;
        } else if (a == "--settle") {
            const char* v = need_value(i, a);
            if (!v || !parse_ms(v, 100, 60000, a, opts.settle)) return false;
        } else if (a == "-w" || a == "--window") {
            const char* v = need_value(i, a);
            if (!v) return false;
            const unsigned long n = std::strtoul(v, nullptr, 10);
            if (n < 64 || n > 4096 || (n & (n - 1)) != 0) {
                std::fprintf(stderr,
                    "%s: --window must be a power of two between 64 and 4096 "
                    "(64, 128, 256, 512, 1024, 2048, 4096)\n", kProgram);
                return false;
            }
            opts.window = static_cast<std::size_t>(n);
        } else {
            std::fprintf(stderr, "%s: unrecognised option '%.*s'\n", kProgram,
                         static_cast<int>(a.size()), a.data());
            std::fprintf(stderr, "try '%s --help'\n", kProgram);
            return false;
        }
    }
    return true;
}

int do_list() {
    const auto devices = epoc::enumerate();
    if (devices.empty()) {
        std::printf("No Emotiv dongle found (USB %04x:%04x).\n",
                    epoc::kVendorId, epoc::kProductId);
        return 1;
    }
    std::printf("%zu Emotiv HID interface(s):\n", devices.size());
    for (std::size_t i = 0; i < devices.size(); ++i) {
        const auto& d = devices[i];
        std::printf("  [%zu] serial %-20s %04x:%04x  interface %d%s\n",
                    i, d.serial.empty() ? "(none reported)" : d.serial.c_str(),
                    d.vendor_id, d.product_id, d.interface_number,
                    d.interface_number == 1 ? "  <- usually the data interface" : "");
        std::printf("       path %s\n", d.path.c_str());
    }
    std::printf(
        "\nThe dongle presents two interfaces; only one streams EEG. Run without\n"
        "--index to probe them automatically.\n");
    return 0;
}

/// Print the AES key a serial produces, and exit.
///
/// Deliberately does not open or stream from the device: key derivation is
/// pure, so this stays useful when the headset is off or absent, which is
/// exactly when you want to check a key by hand.
int do_show_key(const Options& opts) {
    std::string serial;
    if (opts.serial) {
        serial = *opts.serial;
    } else {
        const auto devices = epoc::enumerate();
        if (devices.empty()) {
            std::fprintf(stderr,
                "%s: no dongle connected, so there is no serial to derive from.\n"
                "  Pass one explicitly: %s --show-key --serial SN0123456789ABCD\n",
                kProgram, kProgram);
            return 1;
        }
        serial = devices.front().serial;
        if (serial.empty()) {
            std::fprintf(stderr, "%s: the dongle reported no serial number.\n", kProgram);
            return 1;
        }
    }

    const auto key = epoc::derive_key(serial);
    std::printf("serial: %s\n", serial.c_str());
    std::printf("key:   ");
    for (const auto b : key) std::printf(" %02x", b);
    std::printf("\n");
    return 0;
}

/// Tracks a ~1 second window per channel so the display can show
/// peak-to-peak amplitude, which is the quickest way to tell a live
/// electrode from a flat one.
class PeakWindow {
public:
    void push(const epoc::Frame& f) {
        for (std::size_t c = 0; c < epoc::kChannelCount; ++c) {
            ring_[c][head_] = f.counts[c];
        }
        head_ = (head_ + 1) % kSize;
        if (filled_ < kSize) ++filled_;
    }

    int peak_to_peak(std::size_t channel) const {
        if (filled_ == 0) return 0;
        const auto& r = ring_[channel];
        auto lo = r[0];
        auto hi = r[0];
        for (std::size_t i = 0; i < filled_; ++i) {
            lo = std::min(lo, r[i]);
            hi = std::max(hi, r[i]);
        }
        return hi - lo;
    }

private:
    static constexpr std::size_t kSize = 128;  // one second at 128 Hz
    std::array<std::array<int, kSize>, epoc::kChannelCount> ring_{};
    std::size_t head_ = 0;
    std::size_t filled_ = 0;
};

/// Counts packets the dongle numbered but we never saw. The counter runs
/// 0..127 and is then replaced once per second by a battery reading, which we
/// represent as 128 -- so the sequence is cyclic over 129 values.
class DropCounter {
public:
    void observe(const epoc::Frame& f) {
        const int seq = f.is_battery_frame ? 128 : static_cast<int>(f.counter);
        if (have_prev_) {
            const int expected = (prev_ + 1) % 129;
            if (seq != expected) {
                dropped_ += (seq - expected + 129) % 129;
            }
        }
        prev_ = seq;
        have_prev_ = true;
    }
    long long dropped() const noexcept { return dropped_; }

private:
    int prev_ = 0;
    bool have_prev_ = false;
    long long dropped_ = 0;
};

/// Measured sample rate over a sliding window of arrival timestamps.
class RateMeter {
public:
    void tick(Clock::time_point now) {
        stamps_.push_back(now);
        const auto cutoff = now - std::chrono::seconds{2};
        while (!stamps_.empty() && stamps_.front() < cutoff) stamps_.pop_front();
    }
    double hz() const {
        if (stamps_.size() < 2) return 0.0;
        const auto span = std::chrono::duration<double>(stamps_.back() - stamps_.front()).count();
        if (span <= 0.0) return 0.0;
        return static_cast<double>(stamps_.size() - 1) / span;
    }

private:
    std::deque<Clock::time_point> stamps_;
};

/// Emits OSC for each sample and each recomputed spectrum.
///
/// Addresses are built once up front rather than per message: at 128 Hz with
/// per-channel raw output this runs ~1800 times a second, and rebuilding
/// strings in that loop is pure waste.
class OscStreamer {
public:
    OscStreamer(const std::string& target, const std::string& prefix,
                const epoc::osc::MessageFlags& flags,
                epoc::osc::TimestampFormat ts_format, std::int64_t start_ms)
        : flags_(flags),
          ts_format_(ts_format),
          start_ms_(start_ms),
          prefix_(epoc::osc::normalize_prefix(prefix)) {
        std::string host;
        std::string error;
        std::uint16_t port = 0;
        if (!epoc::osc::parse_endpoint(target, host, port, error)) {
            throw epoc::Error(error);
        }
        sender_ = std::make_unique<epoc::osc::UdpSender>(host, port);

        for (const auto ch : epoc::kChannels) {
            const auto i = static_cast<std::size_t>(ch);
            const std::string leaf = epoc::osc::address_channel(ch);
            raw_addr_[i] = join("raw/" + leaf);
            fft_addr_[i] = join("fft/" + leaf);
        }
        all_addr_ = join("raw/all");
        gyro_x_addr_ = join("gyro/x");
        gyro_y_addr_ = join("gyro/y");
        battery_addr_ = join("battery");
        quality_all_addr_ = join("quality/all");
    }

    /// Append the timestamp in whichever encoding the user selected.
    void add_timestamp(std::int64_t timestamp) {
        switch (ts_format_) {
            case epoc::osc::TimestampFormat::Int64:
                message_.add(timestamp);
                break;
            case epoc::osc::TimestampFormat::Double:
                message_.add(static_cast<double>(timestamp));
                break;
            case epoc::osc::TimestampFormat::Int32Relative:
                // Milliseconds since the stream started. Wraps after ~24 days
                // of continuous streaming, well past any plausible session,
                // and stays inside the only integer type OSC guarantees a
                // receiver understands.
                message_.add(static_cast<std::int32_t>(timestamp - start_ms_));
                break;
            case epoc::osc::TimestampFormat::Timetag:
                message_.add_timetag(epoc::osc::unix_ms_to_timetag(timestamp));
                break;
        }
    }

    /// Per-sample messages. Called at the full 128 Hz sample rate.
    void send_sample(std::int64_t timestamp, const epoc::Frame& f) {
        if (flags_.raw) {
            for (const auto ch : epoc::kChannels) {
                const auto i = static_cast<std::size_t>(ch);
                message_.reset(raw_addr_[i]);
                if (flags_.timestamp) add_timestamp(timestamp);
                message_.add(static_cast<std::int32_t>(f.counts[i]));
                // Sticky: the last known reading for this electrode, not a
                // measurement taken alongside this sample. Attached to every
                // raw message so a receiver never has to join two streams to
                // know whether a value is trustworthy.
                if (flags_.quality) {
                    message_.add(static_cast<std::int32_t>(f.quality[i]));
                }
                sender_->send(message_);
            }
        }
        if (flags_.raw_bundle) {
            message_.reset(all_addr_);
            if (flags_.timestamp) add_timestamp(timestamp);
            for (const auto ch : epoc::kChannels) {
                message_.add(static_cast<std::int32_t>(f.counts[static_cast<std::size_t>(ch)]));
            }
            sender_->send(message_);
        }
        send_quality(timestamp, f);
        if (flags_.gyro) {
            // Two separate addresses rather than one message with two
            // arguments, so a receiver can route each axis independently.
            message_.reset(gyro_x_addr_);
            if (flags_.timestamp) add_timestamp(timestamp);
            message_.add(static_cast<std::int32_t>(f.gyro_x));
            sender_->send(message_);

            message_.reset(gyro_y_addr_);
            if (flags_.timestamp) add_timestamp(timestamp);
            message_.add(static_cast<std::int32_t>(f.gyro_y));
            sender_->send(message_);
        }
        send_battery(timestamp, f);
    }

    /// Every electrode's contact quality at once, on refresh.
    ///
    /// Not sent per sample: a report carries quality for exactly one
    /// electrode, named by byte 0, and only 34 of the 129 counter states name
    /// one at all -- so a fresh reading exists about 34 times a second, and
    /// sending per sample would repeat each one four times over. `Frame`
    /// reports which electrode was refreshed precisely so this can be gated
    /// on it; `quality` itself is sticky and cannot be tested for freshness.
    ///
    /// Every message carries the full set rather than just the electrode that
    /// changed, so a receiver that joins mid-stream is complete within about
    /// half a second. Paired with 'u' because it has the same shape as
    /// /raw/all: one message, 14 values, channel order.
    void send_quality(std::int64_t timestamp, const epoc::Frame& f) {
        if (!flags_.quality || !flags_.raw_bundle || !f.quality_updated) return;

        message_.reset(quality_all_addr_);
        if (flags_.timestamp) add_timestamp(timestamp);
        for (const auto ch : epoc::kChannels) {
            message_.add(static_cast<std::int32_t>(f.quality[static_cast<std::size_t>(ch)]));
        }
        sender_->send(message_);
    }

    /// Battery charge, normalised to 0..1: on change, and every
    /// kBatteryResend regardless.
    ///
    /// Gated on is_battery_frame rather than on Frame::battery changing:
    /// that field is sticky and reads 0 until the first battery frame
    /// arrives, so a plain change test would announce a fictitious flat
    /// battery at startup. The dongle substitutes a reading for the sequence
    /// counter once a second, so the first real value follows the start of
    /// the stream within a second. Gating the resend on the same flag keeps
    /// that guarantee: a level is never sent before one has been measured.
    ///
    /// The resend is what makes this useful to a receiver that joins
    /// mid-stream: on a healthy headset the charge can sit on one value for
    /// hours, and change-only semantics over UDP would leave such a receiver
    /// with nothing at all. At one message every few seconds it is still a
    /// rare message rather than a stream.
    void send_battery(std::int64_t timestamp, const epoc::Frame& f) {
        if (!flags_.battery || !f.is_battery_frame) return;
        const int level = static_cast<int>(f.battery);
        const auto now = Clock::now();
        if (level == last_battery_ && now - last_battery_sent_ < kBatteryResend) return;
        last_battery_ = level;
        last_battery_sent_ = now;

        message_.reset(battery_addr_);
        if (flags_.timestamp) add_timestamp(timestamp);
        message_.add(static_cast<float>(level) / 100.0f);
        sender_->send(message_);
    }

    /// Band messages, sent whenever the spectrum is recomputed.
    void send_bands(std::int64_t timestamp, const epoc::BandAnalyzer& bands) {
        if (!flags_.bands) return;
        for (const auto ch : epoc::kChannels) {
            message_.reset(fft_addr_[static_cast<std::size_t>(ch)]);
            if (flags_.timestamp) add_timestamp(timestamp);
            for (const auto b : epoc::kBands) {
                message_.add(static_cast<float>(bands.power(ch, b)));
            }
            sender_->send(message_);
        }
    }

    bool sends_anything() const noexcept {
        return flags_.raw || flags_.raw_bundle || flags_.bands || flags_.gyro ||
               flags_.battery;
    }
    const epoc::osc::MessageFlags& flags() const noexcept { return flags_; }
    epoc::osc::TimestampFormat timestamp_format() const noexcept { return ts_format_; }
    const std::string& resolved_address() const noexcept {
        return sender_->resolved_address();
    }
    const std::string& prefix() const noexcept { return prefix_; }
    const std::string& endpoint() const noexcept { return sender_->endpoint(); }
    unsigned long long sent() const noexcept { return sender_->sent(); }
    unsigned long long failed() const noexcept { return sender_->failed(); }

private:
    std::string join(const std::string& tail) const {
        return prefix_ == "/" ? "/" + tail : prefix_ + "/" + tail;
    }

    epoc::osc::MessageFlags flags_;
    epoc::osc::TimestampFormat ts_format_;
    std::int64_t start_ms_;
    std::string prefix_;
    std::unique_ptr<epoc::osc::UdpSender> sender_;
    std::array<std::string, epoc::kChannelCount> raw_addr_;
    std::array<std::string, epoc::kChannelCount> fft_addr_;
    std::string all_addr_;
    std::string gyro_x_addr_;
    std::string gyro_y_addr_;
    std::string battery_addr_;
    std::string quality_all_addr_;
    int last_battery_ = -1;  // no reading seen yet
    Clock::time_point last_battery_sent_{};  // epoch: nothing sent yet
    epoc::osc::Message message_;  // reused across sends
};

/// Human-readable name for one candidate, for status messages.
std::string candidate_label(const epoc::DeviceInfo& d) {
    return d.interface_number >= 0
               ? "interface " + std::to_string(d.interface_number)
               : std::string("the selected device");
}

/// Which interfaces to try, honouring an explicit --path or --index.
///
/// An explicit selection pins the list to exactly one interface -- it says
/// *which* device to use. It deliberately does not say whether to wait for it:
/// waiting is handled identically either way, so `--index 1` still tolerates a
/// sleeping headset.
std::vector<epoc::DeviceInfo> resolve_candidates(const Options& opts) {
    if (opts.path) {
        epoc::DeviceInfo d;
        d.path = *opts.path;
        return {d};
    }
    if (opts.index) {
        // Indexes refer to bus order, matching how --list numbers them.
        auto all = epoc::enumerate();
        if (*opts.index >= all.size()) {
            throw epoc::Error("device index " + std::to_string(*opts.index) +
                              " is out of range; " + std::to_string(all.size()) +
                              " Emotiv interface(s) present (see --list)");
        }
        return {all[*opts.index]};
    }
    return epoc::streaming_candidates();
}

/// Try each candidate interface once, returning the first that delivers a
/// packet within `timeout`.
///
/// This is the part that cannot be inferred from the enumeration: both HID
/// interfaces open cleanly, but only one ever delivers a report, so the only
/// way to identify it is to listen.
std::optional<epoc::Device> probe_once(const epoc::OpenOptions& base,
                                       const std::vector<epoc::DeviceInfo>& candidates,
                                       std::chrono::milliseconds timeout,
                                       epoc::Frame& first_frame,
                                       std::string& report,
                                       bool announce) {
    for (const auto& cand : candidates) {
        if (epoc::cli::interrupted()) return std::nullopt;

        epoc::OpenOptions o = base;
        o.path = cand.path;

        std::optional<epoc::Device> dev;
        try {
            dev.emplace(o);
        } catch (const epoc::Error& e) {
            report += "  " + candidate_label(cand) + ": could not open (" + e.what() + ")\n";
            continue;
        }

        if (announce) {
            std::fprintf(stderr, "%s: probing %s ...\n", kProgram,
                         candidate_label(cand).c_str());
        }
        if (dev->read_frame(first_frame, timeout)) {
            if (announce) {
                std::fprintf(stderr, "%s: %s is streaming.\n", kProgram,
                             candidate_label(cand).c_str());
            }
            return dev;
        }
        report += "  " + candidate_label(cand) + ": opened, but sent no data within " +
                  std::to_string(timeout.count()) + " ms\n";
    }
    return std::nullopt;
}

/// Open the dongle and return it only once it has actually produced a packet.
///
/// Returns nullopt if the user interrupted before any data arrived.
///
/// The headset sleeps on its own, so "dongle present but silent" is a routine,
/// recoverable state rather than an error: by default we keep listening until
/// it wakes up, which means the tool can be started before the headset is
/// switched on, in either order. A missing dongle is different -- that is a
/// setup problem waiting will not fix -- so it still fails immediately.
std::optional<epoc::Device> open_streaming(const Options& opts, epoc::Frame& first_frame) {
    epoc::OpenOptions base;
    base.serial_override = opts.serial;

    auto candidates = resolve_candidates(opts);
    if (candidates.empty()) {
        throw epoc::Error("no Emotiv dongle found (looked for USB 21a1:0001); "
                          "is the receiver plugged in?");
    }

    std::string report;
    if (auto dev = probe_once(base, candidates, opts.settle, first_frame, report, true)) {
        return dev;
    }
    if (epoc::cli::interrupted()) return std::nullopt;

    if (!opts.wait) {
        throw epoc::Error(
            "found the dongle but no interface is streaming EEG data.\n" + report +
            "\nThe dongle encrypts and forwards whatever the headset sends, so silence\n"
            "here means the headset itself is not transmitting. Check that it is\n"
            "switched on and charged, and that it is paired with this dongle.\n"
            "Use --settle to wait longer, or --index to force a specific interface.");
    }

    // Waiting mode. Re-resolve candidates on every pass so unplugging and
    // replugging the dongle is picked up too, and use a short per-interface
    // timeout so Ctrl+C stays responsive and the headset is noticed promptly.
    const auto retry_timeout = std::chrono::milliseconds{400};
    const auto started = Clock::now();
    const bool live_status = epoc::cli::Terminal::stderr_is_tty();
    bool status_open = false;
    long long last_note = 0;

    std::fprintf(stderr,
                 "%s: dongle is present but silent -- waiting for the headset.\n"
                 "  Switch it on (and check it is charged); Ctrl+C to give up.\n"
                 "  Use --no-wait to fail immediately instead.\n",
                 kProgram);

    while (!epoc::cli::interrupted()) {
        try {
            candidates = resolve_candidates(opts);
        } catch (const epoc::Error&) {
            // The dongle went away mid-wait; keep looking for it to come back.
            candidates.clear();
        }

        if (!candidates.empty()) {
            std::string ignored;
            if (auto dev = probe_once(base, candidates, retry_timeout, first_frame,
                                      ignored, false)) {
                if (status_open) std::fprintf(stderr, "\n");
                std::fprintf(stderr, "%s: headset is streaming.\n", kProgram);
                return dev;
            }
        }
        if (epoc::cli::interrupted()) break;

        const auto secs = static_cast<long long>(
            std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - started).count());
        const char* what = candidates.empty() ? "waiting for dongle"
                                              : "waiting for headset";
        if (live_status) {
            // Trailing spaces clear the tail of a previously longer line.
            std::fprintf(stderr, "\r  %s... %llds        ", what, secs);
            status_open = true;
        } else if (secs >= last_note + 10) {
            // Not a terminal: no in-place updates, so report periodically
            // instead of spamming a log with one line per pass.
            last_note = secs;
            std::fprintf(stderr, "  %s... %llds\n", what, secs);
        }
        std::fflush(stderr);
    }

    if (status_open) std::fprintf(stderr, "\n");
    return std::nullopt;
}


void draw(const epoc::cli::Terminal& term, const epoc::Device& dev, const epoc::Frame& f,
          const PeakWindow& peaks, const epoc::BandAnalyzer& bands, const DropCounter& drops,
          const RateMeter& rate, long long samples, bool relative,
          const OscStreamer* osc) {
    term.home();

    // Overwrite to end of line on every line so shorter rows cannot leave
    // stale characters behind from a previous frame.
    const char* eol = term.supports_ansi() ? "\x1b[K\n" : "\n";

    std::printf("%s %s -- live EEG%s", kProgram, kVersion, eol);
    std::printf("serial %-18s  band power %s, %.1f s window, %.2f Hz bins%s",
                dev.serial().empty() ? "(unknown)" : dev.serial().c_str(),
                relative ? "as % of channel total" : "in uV^2",
                bands.window_seconds(), bands.resolution_hz(), eol);
    if (osc) {
        std::printf("osc -> %-21s %s [%s]  sent %llu  dropped %llu%s",
                    osc->endpoint().c_str(), osc->prefix().c_str(),
                    epoc::osc::format_message_flags(osc->flags()).c_str(),
                    osc->sent(), osc->failed(), eol);
    }
    std::printf("%s", eol);

    // Build the header with the same field widths as the rows, so the columns
    // cannot drift apart when either is edited.
    std::printf(" %-4s %7s %8s %6s |", "chan", "counts", "uV", "p2p");
    for (const auto b : epoc::kBands) std::printf(" %7s", epoc::band_name(b).data());
    std::printf(" | %-14s %5s%s", "contact", "qual", eol);

    std::printf(" %-4s %7s %8s %6s |", "----", "------", "--", "---");
    for (std::size_t i = 0; i < epoc::kBandCount; ++i) std::printf(" %7s", "-----");
    std::printf(" | %-14s %5s%s", "--------------", "-----", eol);

    for (const auto ch : epoc::kChannels) {
        const auto i = static_cast<std::size_t>(ch);
        std::printf(" %-4s %7d %8.1f %6.1f |",
                    epoc::channel_name(ch).data(),
                    f.counts[i],
                    f.counts[i] * epoc::kMicrovoltsPerCount,
                    peaks.peak_to_peak(i) * epoc::kMicrovoltsPerCount);

        if (!bands.ready()) {
            for (std::size_t b = 0; b < epoc::kBandCount; ++b) std::printf(" %7s", "--");
        } else if (relative) {
            const double total = bands.total_power(ch);
            for (const auto b : epoc::kBands) {
                const double pct = (total > 0.0) ? 100.0 * bands.power(ch, b) / total : 0.0;
                std::printf(" %6.1f%%", pct);
            }
        } else {
            for (const auto b : epoc::kBands) {
                std::printf(" %7.1f", bands.power(ch, b));
            }
        }

        std::printf(" | %s %5d%s",
                    epoc::cli::bar(f.quality[i], kQualityGood, 14).c_str(),
                    f.quality[i], eol);
    }

    std::printf("%s", eol);
    if (!bands.ready()) {
        const double remaining =
            static_cast<double>(bands.window_samples() - bands.filled()) /
            epoc::kNominalSampleRateHz;
        std::printf(" collecting spectrum: %.1f s to go%s", remaining, eol);
    } else {
        std::printf(" battery %3u%%    gyro x %+4d  y %+4d    seq %3u%s",
                    static_cast<unsigned>(f.battery), f.gyro_x, f.gyro_y,
                    static_cast<unsigned>(f.counter), eol);
    }
    std::printf(" samples %-10lld drops %-8lld  rate %6.1f Hz%s",
                samples, drops.dropped(), rate.hz(), eol);
    std::printf("%s", eol);

    // The table is a fixed ~100 columns. Say so rather than letting it wrap
    // into an unreadable mess with no explanation.
    const int cols = epoc::cli::Terminal::width();
    if (cols > 0 && cols < kTableWidth) {
        std::printf(" note: terminal is %d columns, the table needs %d -- widen the"
                    " window to stop it wrapping%s", cols, kTableWidth, eol);
    }
    std::printf(" Ctrl+C to quit%s", eol);

    std::fflush(stdout);
}

/// Append-only output for when stdout is not a terminal (piped or redirected).
/// One compact line per refresh interval, so a redirect stays readable.
void draw_plain(const epoc::Frame& f, const epoc::BandAnalyzer& bands, const RateMeter& rate,
                long long samples, long long dropped) {
    std::printf("samples=%lld drops=%lld rate=%.1fHz seq=%u batt=%u%%",
                samples, dropped, rate.hz(), static_cast<unsigned>(f.counter),
                static_cast<unsigned>(f.battery));
    for (const auto ch : epoc::kChannels) {
        std::printf(" %s=%d", epoc::channel_name(ch).data(),
                    f.counts[static_cast<std::size_t>(ch)]);
    }
    if (bands.ready()) {
        for (const auto ch : epoc::kChannels) {
            for (const auto b : epoc::kBands) {
                std::printf(" %s.%s=%.1f", epoc::channel_name(ch).data(),
                            epoc::band_name(b).data(), bands.power(ch, b));
            }
        }
    }
    std::printf("\n");
    std::fflush(stdout);
}

int run(const Options& opts) {
    // Armed before opening, because open_streaming() may sit and wait for the
    // headset and Ctrl+C has to get us out of that.
    epoc::cli::install_interrupt_handler();

    // Set up OSC before touching the hardware: a bad destination must fail
    // immediately rather than after a device probe, or worse, after sitting
    // in the wait-for-headset loop.
    const epoc::osc::MonotonicUnixClock osc_clock;
    std::optional<OscStreamer> osc;
    if (opts.osc_target) {
        epoc::osc::MessageFlags flags;
        std::string error;
        // Already validated during argument parsing.
        epoc::osc::parse_message_flags(opts.osc_messages, flags, error);
        osc.emplace(*opts.osc_target, opts.osc_prefix, flags, opts.osc_timestamp,
                    osc_clock.now_ms());
        std::fprintf(stderr,
                     "%s: streaming OSC to %s (%s), prefix %s, messages [%s], timestamp %s\n",
                     kProgram, osc->endpoint().c_str(), osc->resolved_address().c_str(),
                     osc->prefix().c_str(), epoc::osc::format_message_flags(flags).c_str(),
                     epoc::osc::timestamp_format_name(opts.osc_timestamp).data());
        if (!osc->sends_anything()) {
            std::fprintf(stderr,
                "%s: warning -- OSC is enabled but no message type is selected;\n"
                "  add r, b, u, g or y to --osc-messages"
                " (q only modifies r and u).\n", kProgram);
        }
    }

    epoc::Frame frame;  // reused: carries sticky battery/quality across reads
    auto opened = open_streaming(opts, frame);
    if (!opened) {
        std::fprintf(stderr, "%s: cancelled while waiting for the headset.\n", kProgram);
        return 0;
    }
    epoc::Device dev = std::move(*opened);

    epoc::cli::Terminal term;
    const bool interactive = term.supports_ansi();

    if (interactive) {
        term.clear();
        term.show_cursor(false);
    }

    PeakWindow peaks;
    epoc::BandAnalyzer bands(opts.window);
    DropCounter drops;
    RateMeter rate;
    long long samples = 0;
    long long timeouts = 0;

    auto last_draw = Clock::now() - opts.refresh;

    while (!epoc::cli::interrupted()) {
        if (!dev.read_frame(frame, std::chrono::milliseconds{500})) {
            // A mid-session gap is not fatal: the headset may have been
            // switched off or moved out of range while the dongle stays open.
            ++timeouts;
            if (timeouts == 4) {
                if (interactive) term.clear();
                std::fprintf(stderr,
                    "%s: no data for ~2s -- headset off, out of range, or asleep.\n"
                    "  Still listening; Ctrl+C to quit.\n",
                    kProgram);
            }
            continue;
        }
        timeouts = 0;

        ++samples;
        drops.observe(frame);
        rate.tick(Clock::now());
        peaks.push(frame);
        bands.push(frame);

        // One timestamp per sample, shared by every message describing it.
        const std::int64_t timestamp = osc ? osc_clock.now_ms() : 0;
        if (osc) osc->send_sample(timestamp, frame);

        const auto now = Clock::now();
        if (now - last_draw >= opts.refresh) {
            last_draw = now;
            // Recompute the spectra once per redraw rather than once per
            // sample: 14 FFTs at 128 Hz would be wasted work nobody sees.
            bands.compute();
            if (osc && bands.ready()) osc->send_bands(timestamp, bands);
            if (interactive) {
                draw(term, dev, frame, peaks, bands, drops, rate, samples, opts.relative,
                     osc ? &*osc : nullptr);
            } else {
                draw_plain(frame, bands, rate, samples, drops.dropped());
            }
        }
    }

    if (interactive) {
        term.show_cursor(true);
        std::printf("\n");
    }
    std::fprintf(stderr, "%s: stopped after %lld samples (%lld dropped).\n",
                 kProgram, samples, drops.dropped());
    if (osc) {
        std::fprintf(stderr, "%s: sent %llu OSC messages to %s (%llu failed).\n",
                     kProgram, osc->sent(), osc->endpoint().c_str(), osc->failed());
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options opts;
    if (!parse_args(argc, argv, opts)) return 2;

    if (opts.help) {
        print_usage(stdout);
        return 0;
    }
    if (opts.version) {
        std::printf("%s %s\n", kProgram, kVersion);
        return 0;
    }
    if (opts.list) {
        return do_list();
    }

    try {
        if (opts.show_key) {
            return do_show_key(opts);
        }
        return run(opts);
    } catch (const epoc::Error& e) {
        std::fprintf(stderr, "%s: %s\n", kProgram, e.what());
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s: unexpected error: %s\n", kProgram, e.what());
        return 1;
    }
}
