// Spectral analysis for EPOC channel data: FFT and standard EEG band powers.

#ifndef XAVIER_EPOC_DSP_HPP
#define XAVIER_EPOC_DSP_HPP

#include "epoc/epoc.hpp"

#include <array>
#include <complex>
#include <cstddef>
#include <string_view>
#include <vector>

namespace epoc {

/// The conventional clinical EEG frequency bands.
enum class Band : int { Delta = 0, Theta, Alpha, Beta, Gamma };

inline constexpr std::size_t kBandCount = 5;

inline constexpr std::array<Band, kBandCount> kBands = {
    Band::Delta, Band::Theta, Band::Alpha, Band::Beta, Band::Gamma};

/// Band label, e.g. "alpha". Never null, valid for the program's lifetime.
std::string_view band_name(Band b) noexcept;

/// Half-open frequency range [low_hz, high_hz) for a band.
struct BandRange {
    double low_hz;
    double high_hz;
};

/// Band edges. These are the standard ranges: delta 0.5-4, theta 4-8,
/// alpha 8-13, beta 13-30, gamma 30-45 Hz.
///
/// Gamma stops at 45 Hz rather than the 64 Hz Nyquist limit because the EPOC's
/// own analogue front end band-limits to roughly 0.16-43 Hz; there is no real
/// signal above that, only the filter roll-off and mains hum.
BandRange band_range(Band b) noexcept;

/// In-place iterative radix-2 Cooley-Tukey FFT.
/// `data.size()` must be a power of two.
void fft(std::vector<std::complex<double>>& data);

/// Sliding-window band power for all 14 channels.
///
/// Feed it every frame; call compute() when you want fresh numbers (typically
/// once per display refresh, not once per sample) and then read power().
/// Separating the two keeps the 128 Hz ingest path cheap.
class BandAnalyzer {
public:
    /// `window_samples` must be a power of two, at least 64. It trades
    /// frequency resolution against latency: the default 256 samples is 2 s of
    /// data at 128 Hz, giving 0.5 Hz bins, which is fine enough to separate
    /// delta (0.5-4 Hz) from theta.
    explicit BandAnalyzer(std::size_t window_samples = 256,
                          double sample_rate_hz = kNominalSampleRateHz);

    /// Append one sample per channel. O(channels).
    void push(const Frame& f);

    /// True once a full window has been collected; power() is zero until then.
    bool ready() const noexcept { return filled_ >= window_; }
    std::size_t filled() const noexcept { return filled_; }
    std::size_t window_samples() const noexcept { return window_; }
    double window_seconds() const noexcept {
        return static_cast<double>(window_) / sample_rate_;
    }
    /// Width of one FFT bin, in Hz.
    double resolution_hz() const noexcept {
        return sample_rate_ / static_cast<double>(window_);
    }

    /// Recompute band powers over the current window. No-op until ready().
    void compute();

    /// Band power in uV^2, as of the last compute().
    double power(Channel c, Band b) const noexcept;
    /// Sum of the five band powers for a channel, in uV^2.
    double total_power(Channel c) const noexcept;

private:
    std::size_t window_;
    double sample_rate_;

    /// Hann window, and S2 = sum(w^2), the normalisation that makes the
    /// integrated power spectral density independent of the window shape.
    std::vector<double> hann_;
    double window_power_sum_ = 0.0;

    /// One ring buffer per channel, in microvolts.
    std::array<std::vector<double>, kChannelCount> ring_{};
    std::size_t head_ = 0;
    std::size_t filled_ = 0;

    std::array<std::array<double, kBandCount>, kChannelCount> power_{};
    std::vector<std::complex<double>> scratch_;
};

}  // namespace epoc

#endif  // XAVIER_EPOC_DSP_HPP
