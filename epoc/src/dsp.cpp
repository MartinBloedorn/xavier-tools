#include "epoc/dsp.hpp"

#include <cmath>

namespace epoc {
namespace {

constexpr double kPi = 3.14159265358979323846;

constexpr std::string_view kBandNames[kBandCount] = {
    "delta", "theta", "alpha", "beta", "gamma"};

// Standard clinical band edges, half-open [low, high).
constexpr BandRange kBandRanges[kBandCount] = {
    /* delta */ {0.5, 4.0},
    /* theta */ {4.0, 8.0},
    /* alpha */ {8.0, 13.0},
    /* beta  */ {13.0, 30.0},
    /* gamma */ {30.0, 45.0},
};

bool is_power_of_two(std::size_t n) noexcept {
    return n != 0 && (n & (n - 1)) == 0;
}

}  // namespace

std::string_view band_name(Band b) noexcept {
    const auto i = static_cast<std::size_t>(b);
    return i < kBandCount ? kBandNames[i] : std::string_view{"?"};
}

BandRange band_range(Band b) noexcept {
    const auto i = static_cast<std::size_t>(b);
    return i < kBandCount ? kBandRanges[i] : BandRange{0.0, 0.0};
}

void fft(std::vector<std::complex<double>>& data) {
    const std::size_t n = data.size();
    if (n < 2) return;
    if (!is_power_of_two(n)) {
        throw Error("fft() requires a power-of-two size, got " + std::to_string(n));
    }

    // Bit-reversal permutation.
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) std::swap(data[i], data[j]);
    }

    // Butterflies, stage by stage.
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double theta = -2.0 * kPi / static_cast<double>(len);
        const std::complex<double> step(std::cos(theta), std::sin(theta));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const std::complex<double> u = data[i + k];
                const std::complex<double> v = data[i + k + len / 2] * w;
                data[i + k] = u + v;
                data[i + k + len / 2] = u - v;
                w *= step;
            }
        }
    }
}

BandAnalyzer::BandAnalyzer(std::size_t window_samples, double sample_rate_hz)
    : window_(window_samples), sample_rate_(sample_rate_hz) {
    if (!is_power_of_two(window_) || window_ < 64) {
        throw Error("BandAnalyzer window must be a power of two and at least 64, got " +
                    std::to_string(window_samples));
    }
    if (!(sample_rate_ > 0.0)) {
        throw Error("BandAnalyzer sample rate must be positive");
    }

    // Hann window. Tapering the ends suppresses the spectral leakage that a
    // rectangular window would smear across every band, which matters here
    // because delta is adjacent to the (removed) DC bin and would otherwise
    // absorb leakage from any residual drift.
    hann_.resize(window_);
    for (std::size_t i = 0; i < window_; ++i) {
        hann_[i] = 0.5 * (1.0 - std::cos(2.0 * kPi * static_cast<double>(i) /
                                         static_cast<double>(window_ - 1)));
        window_power_sum_ += hann_[i] * hann_[i];
    }

    for (auto& r : ring_) r.assign(window_, 0.0);
    scratch_.resize(window_);
}

void BandAnalyzer::push(const Frame& f) {
    for (std::size_t c = 0; c < kChannelCount; ++c) {
        ring_[c][head_] = static_cast<double>(f.counts[c]) * kMicrovoltsPerCount;
    }
    head_ = (head_ + 1) % window_;
    if (filled_ < window_) ++filled_;
}

void BandAnalyzer::compute() {
    if (!ready()) return;

    const double df = resolution_hz();
    const std::size_t half = window_ / 2;

    for (std::size_t c = 0; c < kChannelCount; ++c) {
        const auto& ring = ring_[c];

        // head_ is the next write slot, so it is also the oldest sample:
        // walking forward from it yields the window in chronological order.
        double mean = 0.0;
        for (std::size_t i = 0; i < window_; ++i) {
            mean += ring[(head_ + i) % window_];
        }
        mean /= static_cast<double>(window_);

        // Remove the mean before windowing. EPOC counts sit on a large DC
        // offset (~8400 counts, ~4.3 mV); left in, it would dwarf every real
        // rhythm and leak straight into delta.
        for (std::size_t i = 0; i < window_; ++i) {
            const double x = ring[(head_ + i) % window_] - mean;
            scratch_[i] = std::complex<double>(x * hann_[i], 0.0);
        }

        fft(scratch_);

        auto& out = power_[c];
        out.fill(0.0);

        // One-sided power spectral density, in uV^2/Hz:
        //   PSD_k = 2 * |X_k|^2 / (fs * S2)
        // Integrating over a band (multiplying by the bin width df) gives the
        // band power in uV^2. Normalising by S2 = sum(w^2) is what makes that
        // integral independent of the window function.
        //
        // Bin 0 (DC) and bin N/2 (Nyquist) are skipped: DC was subtracted out,
        // and Nyquist lies far above the EPOC's analogue bandwidth.
        const double scale = 2.0 / (sample_rate_ * window_power_sum_);
        for (std::size_t k = 1; k < half; ++k) {
            const double f_hz = static_cast<double>(k) * df;
            if (f_hz < kBandRanges[0].low_hz) continue;
            if (f_hz >= kBandRanges[kBandCount - 1].high_hz) break;

            const double psd = scale * std::norm(scratch_[k]);
            for (std::size_t b = 0; b < kBandCount; ++b) {
                if (f_hz >= kBandRanges[b].low_hz && f_hz < kBandRanges[b].high_hz) {
                    out[b] += psd * df;
                    break;
                }
            }
        }
    }
}

double BandAnalyzer::power(Channel c, Band b) const noexcept {
    const auto ci = static_cast<std::size_t>(c);
    const auto bi = static_cast<std::size_t>(b);
    if (ci >= kChannelCount || bi >= kBandCount) return 0.0;
    return power_[ci][bi];
}

double BandAnalyzer::total_power(Channel c) const noexcept {
    const auto ci = static_cast<std::size_t>(c);
    if (ci >= kChannelCount) return 0.0;
    double sum = 0.0;
    for (const double p : power_[ci]) sum += p;
    return sum;
}

}  // namespace epoc
