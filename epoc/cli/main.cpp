// xavier-tools `epoc`: read the Emotiv EPOC's 14 EEG channels in real time.

#include "epoc/epoc.hpp"

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
constexpr const char* kVersion = "0.1.0";

/// Contact quality full-scale for the bar display. emokit's header notes that
/// values above 4000 indicate a good contact; the units are otherwise
/// undocumented, so this is a display convention, not a calibrated threshold.
constexpr int kQualityGood = 4000;

struct Options {
    bool list = false;
    bool help = false;
    bool version = false;
    bool show_key = false;
    std::optional<std::size_t> index;
    std::optional<std::string> path;
    std::optional<std::string> serial;
    std::optional<epoc::DeviceVariant> variant;
    std::chrono::milliseconds refresh{50};
    /// How long to wait for a candidate interface to produce its first
    /// packet before moving on to the next one.
    std::chrono::milliseconds settle{2000};
};

void print_usage(std::FILE* out) {
    std::fprintf(out,
        "%s %s -- read Emotiv EPOC EEG channels in real time\n"
        "\n"
        "usage: %s [options]\n"
        "\n"
        "options:\n"
        "  -l, --list            list connected Emotiv dongles and exit\n"
        "  -i, --index N         open the Nth dongle (default: 0)\n"
        "      --path PATH       open a specific HID path (see --list)\n"
        "      --serial SN       override the serial used for key derivation\n"
        "      --consumer        force the consumer key layout\n"
        "      --research        force the research key layout\n"
        "      --show-key        print the derived AES key and exit (debugging)\n"
        "      --refresh MS      display refresh interval, ms (default: 50)\n"
        "      --settle MS       per-interface probe timeout, ms (default: 2000)\n"
        "  -h, --help            show this help and exit\n"
        "  -V, --version         show version and exit\n"
        "\n"
        "The dongle decrypts nothing by itself: data is AES-128 encrypted\n"
        "against a key derived from the dongle's serial number. If channels\n"
        "read as noise with a plausible-looking serial, try the other key\n"
        "layout (--consumer / --research).\n",
        kProgram, kVersion, kProgram);
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
        } else if (a == "--consumer") {
            opts.variant = epoc::DeviceVariant::Consumer;
        } else if (a == "--research") {
            opts.variant = epoc::DeviceVariant::Research;
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
            if (!v) return false;
            const long ms = std::strtol(v, nullptr, 10);
            if (ms < 1 || ms > 10000) {
                std::fprintf(stderr, "%s: --refresh must be between 1 and 10000 ms\n", kProgram);
                return false;
            }
            opts.refresh = std::chrono::milliseconds{ms};
        } else if (a == "--settle") {
            const char* v = need_value(i, a);
            if (!v) return false;
            const long ms = std::strtol(v, nullptr, 10);
            if (ms < 100 || ms > 60000) {
                std::fprintf(stderr, "%s: --settle must be between 100 and 60000 ms\n", kProgram);
                return false;
            }
            opts.settle = std::chrono::milliseconds{ms};
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

/// Print the AES key the given serial and variant produce, and exit.
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

    // Without an open device we cannot read the feature report, so show both
    // layouts unless the user pinned one.
    std::printf("serial: %s\n", serial.c_str());
    const auto show = [&](epoc::DeviceVariant v) {
        const auto key = epoc::derive_key(serial, v);
        std::printf("%-9s", epoc::variant_name(v).data());
        for (const auto b : key) std::printf(" %02x", b);
        std::printf("\n");
    };
    if (opts.variant) {
        show(*opts.variant);
    } else {
        std::printf("(variant not detected -- showing both layouts)\n");
        show(epoc::DeviceVariant::Consumer);
        show(epoc::DeviceVariant::Research);
    }
    return 0;
}

/// Tracks a ~1 second window per channel so the display can show
/// peak-to-peak amplitude, which is the quickest way to tell a live
/// electrode from a flat one.
class Window {
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

void draw(const epoc::cli::Terminal& term, const epoc::Device& dev, const epoc::Frame& f,
          const Window& window, const DropCounter& drops, const RateMeter& rate,
          long long samples, bool variant_forced) {
    term.home();

    // Overwrite to end of line on every line so shorter rows cannot leave
    // stale characters behind from a previous frame.
    const char* eol = term.supports_ansi() ? "\x1b[K\n" : "\n";

    std::printf("%s %s -- live EEG%s", kProgram, kVersion, eol);
    std::printf("serial %-18s variant %s%s%s",
                dev.serial().empty() ? "(unknown)" : dev.serial().c_str(),
                epoc::variant_name(dev.variant()).data(),
                variant_forced ? " (forced)" : " (detected)", eol);
    std::printf("%s", eol);

    std::printf(" chan      counts           uV     p2p/s uV   contact quality%s", eol);
    std::printf(" ----      ------           --     --------   ---------------%s", eol);

    for (const auto ch : epoc::kChannels) {
        const auto i = static_cast<std::size_t>(ch);
        const int p2p = window.peak_to_peak(i);
        std::printf(" %-4s    %7d    %9.1f    %8.1f   %s %5d%s",
                    epoc::channel_name(ch).data(),
                    f.counts[i],
                    f.counts[i] * epoc::kMicrovoltsPerCount,
                    p2p * epoc::kMicrovoltsPerCount,
                    epoc::cli::bar(f.quality[i], kQualityGood, 14).c_str(),
                    f.quality[i],
                    eol);
    }

    std::printf("%s", eol);
    std::printf(" battery %3u%%    gyro x %+4d  y %+4d    seq %3u%s",
                static_cast<unsigned>(f.battery), f.gyro_x, f.gyro_y,
                static_cast<unsigned>(f.counter), eol);
    std::printf(" samples %-10lld drops %-8lld  rate %6.1f Hz%s",
                samples, drops.dropped(), rate.hz(), eol);
    std::printf("%s", eol);
    std::printf(" Ctrl+C to quit%s", eol);

    std::fflush(stdout);
}

/// Append-only output for when stdout is not a terminal (piped or redirected).
/// One compact line per refresh interval, so a redirect stays readable.
void draw_plain(const epoc::Frame& f, const RateMeter& rate, long long samples,
                long long dropped) {
    std::printf("samples=%lld drops=%lld rate=%.1fHz seq=%u batt=%u%%",
                samples, dropped, rate.hz(), static_cast<unsigned>(f.counter),
                static_cast<unsigned>(f.battery));
    for (const auto ch : epoc::kChannels) {
        std::printf(" %s=%d", epoc::channel_name(ch).data(),
                    f.counts[static_cast<std::size_t>(ch)]);
    }
    std::printf("\n");
    std::fflush(stdout);
}

/// Open the dongle and return it only once it has actually produced a packet.
///
/// This is the part that cannot be inferred from the enumeration: both HID
/// interfaces open cleanly, but only one ever delivers a report. So when the
/// user has not named an interface explicitly, try each candidate in turn and
/// keep the first that speaks within `settle`.
epoc::Device open_streaming(const Options& opts, epoc::Frame& first_frame) {
    epoc::OpenOptions base;
    base.serial_override = opts.serial;
    base.variant_override = opts.variant;

    // An explicit --path or --index is an instruction, not a hint: honour it
    // exactly and let the read loop report silence if there is any.
    if (opts.path || opts.index) {
        epoc::OpenOptions o = base;
        o.path = opts.path;
        o.index = opts.index.value_or(0);
        return epoc::Device(o);
    }

    const auto candidates = epoc::streaming_candidates();
    if (candidates.empty()) {
        throw epoc::Error("no Emotiv dongle found (looked for USB 21a1:0001); "
                          "is the receiver plugged in?");
    }

    std::string report;
    for (const auto& cand : candidates) {
        epoc::OpenOptions o = base;
        o.path = cand.path;

        std::optional<epoc::Device> dev;
        try {
            dev.emplace(o);
        } catch (const epoc::Error& e) {
            report += "  interface " + std::to_string(cand.interface_number) +
                      ": could not open (" + e.what() + ")\n";
            continue;
        }

        std::fprintf(stderr, "%s: probing interface %d ...\n", kProgram, cand.interface_number);
        if (dev->read_frame(first_frame, opts.settle)) {
            std::fprintf(stderr, "%s: interface %d is streaming.\n", kProgram,
                         cand.interface_number);
            return std::move(*dev);
        }
        report += "  interface " + std::to_string(cand.interface_number) +
                  ": opened, but sent no data within " +
                  std::to_string(opts.settle.count()) + " ms\n";
    }

    throw epoc::Error(
        "found the dongle but no interface is streaming EEG data.\n" + report +
        "\nThe dongle encrypts and forwards whatever the headset sends, so silence\n"
        "here means the headset itself is not transmitting. Check that it is\n"
        "switched on and charged, and that it is paired with this dongle.\n"
        "Use --settle to wait longer, or --index to force a specific interface.");
}

int run(const Options& opts) {
    epoc::Frame frame;  // reused: carries sticky battery/quality across reads
    epoc::Device dev = open_streaming(opts, frame);

    epoc::cli::install_interrupt_handler();
    epoc::cli::Terminal term;
    const bool interactive = term.supports_ansi();

    if (interactive) {
        term.clear();
        term.show_cursor(false);
    }

    Window window;
    DropCounter drops;
    RateMeter rate;
    long long samples = 0;

    // The probe in open_streaming() already consumed one real packet (unless
    // an explicit --index/--path skipped probing, in which case `frame` is
    // still default-constructed and contributes nothing but a zero row).
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
        window.push(frame);

        const auto now = Clock::now();
        if (now - last_draw >= opts.refresh) {
            last_draw = now;
            if (interactive) {
                draw(term, dev, frame, window, drops, rate, samples, opts.variant.has_value());
            } else {
                draw_plain(frame, rate, samples, drops.dropped());
            }
        }
    }

    if (interactive) {
        term.show_cursor(true);
        std::printf("\n");
    }
    std::fprintf(stderr, "%s: stopped after %lld samples (%lld dropped).\n",
                 kProgram, samples, drops.dropped());
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
    if (opts.show_key) {
        try {
            return do_show_key(opts);
        } catch (const epoc::Error& e) {
            std::fprintf(stderr, "%s: %s\n", kProgram, e.what());
            return 1;
        }
    }

    try {
        return run(opts);
    } catch (const epoc::Error& e) {
        std::fprintf(stderr, "%s: %s\n", kProgram, e.what());
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s: unexpected error: %s\n", kProgram, e.what());
        return 1;
    }
}
