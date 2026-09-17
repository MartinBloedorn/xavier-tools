// Tests for the FFT and EEG band-power analysis.
//
// The important property here is absolute calibration: a sinusoid of known
// amplitude must come back with the right power in the right band, in uV^2.
// Getting the window normalisation wrong is easy and produces numbers that
// look plausible but are off by a constant factor, so these tests pin it
// against the analytic answer rather than against a previous run.

#include "epoc/dsp.hpp"

#include "check.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <string>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

/// Typical EPOC DC offset, in raw counts. Present in every test signal so the
/// mean-removal path is always exercised.
constexpr int kDcOffset = 8400;

/// Feed `analyzer` a synthetic signal built from count-amplitude sinusoids.
/// Returns the microvolt amplitude of each component, for expected-power maths.
struct Component {
    double freq_hz;
    double count_amplitude;
};

void feed(epoc::BandAnalyzer& analyzer, const std::vector<Component>& components,
          std::size_t samples, double sample_rate = epoc::kNominalSampleRateHz) {
    for (std::size_t n = 0; n < samples; ++n) {
        double value = static_cast<double>(kDcOffset);
        for (const auto& comp : components) {
            value += comp.count_amplitude *
                     std::sin(2.0 * kPi * comp.freq_hz * static_cast<double>(n) / sample_rate);
        }
        // Quantise to integer counts, as the hardware does.
        const int counts = static_cast<int>(std::lround(value));

        epoc::Frame f;
        f.counts.fill(counts);
        analyzer.push(f);
    }
}

/// Expected band power, uV^2, for a sinusoid of the given count amplitude.
/// A sine of amplitude A has mean power A^2/2.
double expected_power(double count_amplitude) {
    const double uv = count_amplitude * epoc::kMicrovoltsPerCount;
    return uv * uv / 2.0;
}

void test_band_table() {
    tst::section("band table");

    tst::check(epoc::kBands.size() == epoc::kBandCount, "5 bands enumerated");

    bool indexed = true;
    for (std::size_t i = 0; i < epoc::kBands.size(); ++i) {
        if (static_cast<std::size_t>(epoc::kBands[i]) != i) indexed = false;
    }
    tst::check(indexed, "kBands is indexed by enumerator value");

    // Names must be unique or the display would mislabel columns.
    std::vector<std::string> names;
    for (const auto b : epoc::kBands) names.emplace_back(epoc::band_name(b));
    std::vector<std::string> sorted = names;
    std::sort(sorted.begin(), sorted.end());
    tst::check(std::unique(sorted.begin(), sorted.end()) == sorted.end(),
               "all band names are distinct");

    // Ranges must ascend and tile without gaps or overlap, otherwise some
    // frequencies would be counted twice or dropped silently.
    bool contiguous = true;
    bool ascending = true;
    for (std::size_t i = 0; i < epoc::kBandCount; ++i) {
        const auto r = epoc::band_range(epoc::kBands[i]);
        if (!(r.low_hz < r.high_hz)) ascending = false;
        if (i > 0) {
            const auto prev = epoc::band_range(epoc::kBands[i - 1]);
            if (prev.high_hz != r.low_hz) contiguous = false;
        }
    }
    tst::check(ascending, "every band range is non-empty and ascending");
    tst::check(contiguous, "band ranges tile contiguously with no gaps");

    tst::check(epoc::band_range(epoc::Band::Alpha).low_hz == 8.0 &&
                   epoc::band_range(epoc::Band::Alpha).high_hz == 13.0,
               "alpha is 8-13 Hz");

    // Gamma must stay below Nyquist for the 128 Hz sample rate.
    tst::check(epoc::band_range(epoc::Band::Gamma).high_hz <
                   epoc::kNominalSampleRateHz / 2.0,
               "gamma upper edge is below Nyquist");
}

void test_fft() {
    tst::section("FFT");

    // A unit impulse at n=0 transforms to a flat unit spectrum.
    std::vector<std::complex<double>> impulse(64, {0.0, 0.0});
    impulse[0] = {1.0, 0.0};
    epoc::fft(impulse);
    bool flat = true;
    for (const auto& v : impulse) {
        if (std::abs(std::abs(v) - 1.0) > 1e-9) flat = false;
    }
    tst::check(flat, "impulse transforms to a flat spectrum");

    // A constant signal puts all its energy in bin 0.
    std::vector<std::complex<double>> dc(64, {2.0, 0.0});
    epoc::fft(dc);
    tst::check(std::abs(dc[0].real() - 128.0) < 1e-9, "DC lands in bin 0");
    bool rest_zero = true;
    for (std::size_t k = 1; k < dc.size(); ++k) {
        if (std::abs(dc[k]) > 1e-9) rest_zero = false;
    }
    tst::check(rest_zero, "DC leaves all other bins empty");

    // A sinusoid at an exact bin frequency peaks in that bin and its mirror.
    const std::size_t n = 128;
    const std::size_t k0 = 10;
    std::vector<std::complex<double>> sine(n);
    for (std::size_t i = 0; i < n; ++i) {
        sine[i] = {std::sin(2.0 * kPi * static_cast<double>(k0) * static_cast<double>(i) /
                            static_cast<double>(n)),
                   0.0};
    }
    epoc::fft(sine);
    std::size_t peak = 0;
    double peak_mag = -1.0;
    for (std::size_t k = 0; k < n / 2; ++k) {
        if (std::abs(sine[k]) > peak_mag) {
            peak_mag = std::abs(sine[k]);
            peak = k;
        }
    }
    tst::check(peak == k0, "sinusoid peaks in its own bin");

    bool threw = false;
    try {
        std::vector<std::complex<double>> bad(100);
        epoc::fft(bad);
    } catch (const epoc::Error&) {
        threw = true;
    }
    tst::check(threw, "non-power-of-two size is rejected");
}

void test_analyzer_lifecycle() {
    tst::section("analyzer lifecycle");

    epoc::BandAnalyzer a(256);
    tst::check(!a.ready(), "not ready before the window fills");
    tst::check(a.window_samples() == 256, "window size honoured");
    tst::check_near(a.window_seconds(), 2.0, 1e-9, "256 samples at 128 Hz is 2 s");
    tst::check_near(a.resolution_hz(), 0.5, 1e-9, "2 s window gives 0.5 Hz bins");

    feed(a, {{10.0, 500.0}}, 255);
    tst::check(!a.ready(), "still not ready one sample short");
    a.compute();
    tst::check(a.total_power(epoc::Channel::AF3) == 0.0,
               "compute() is a no-op before the window fills");

    feed(a, {{10.0, 500.0}}, 1);
    tst::check(a.ready(), "ready once the window is full");

    bool threw = false;
    try {
        epoc::BandAnalyzer bad(300);
    } catch (const epoc::Error&) {
        threw = true;
    }
    tst::check(threw, "non-power-of-two window is rejected");

    threw = false;
    try {
        epoc::BandAnalyzer tiny(32);
    } catch (const epoc::Error&) {
        threw = true;
    }
    tst::check(threw, "absurdly small window is rejected");
}

void test_dc_rejection() {
    tst::section("DC rejection");

    // A pure DC signal at the EPOC's usual offset must produce no band power.
    // Without mean removal this would dump an enormous value into delta.
    epoc::BandAnalyzer a(256);
    feed(a, {}, 256);
    a.compute();

    double worst = 0.0;
    for (const auto ch : epoc::kChannels) {
        worst = std::max(worst, a.total_power(ch));
    }
    tst::check(worst < 1e-6, "constant input yields essentially zero band power");
}

void test_absolute_calibration() {
    tst::section("absolute calibration");

    // 10 Hz sits mid-alpha, and at 0.5 Hz bins it falls on an exact bin, so
    // the Hann main lobe stays entirely inside the alpha range.
    const double amplitude = 1000.0;  // counts
    epoc::BandAnalyzer a(256);
    feed(a, {{10.0, amplitude}}, 256);
    a.compute();

    const double want = expected_power(amplitude);
    const double got = a.power(epoc::Channel::AF3, epoc::Band::Alpha);

    // Integrating the one-sided PSD over the band must recover A^2/2.
    tst::check_near(got, want, 0.03, "10 Hz sinusoid recovers A^2/2 in alpha");

    // And it must land in alpha, not smeared across neighbours.
    const double total = a.total_power(epoc::Channel::AF3);
    tst::check(total > 0.0 && got / total > 0.95,
               "at least 95% of the power is in alpha");

    // Every channel sees the same synthetic signal, so all must agree.
    bool uniform = true;
    for (const auto ch : epoc::kChannels) {
        if (std::abs(a.power(ch, epoc::Band::Alpha) - got) > 1e-9) uniform = false;
    }
    tst::check(uniform, "all 14 channels decode identically");
}

void test_band_assignment() {
    tst::section("band assignment");

    // One tone per band, each at an exact bin, comfortably inside its range.
    const std::array<std::pair<double, epoc::Band>, 5> cases = {{
        {2.0, epoc::Band::Delta},
        {6.0, epoc::Band::Theta},
        {10.0, epoc::Band::Alpha},
        {20.0, epoc::Band::Beta},
        {38.0, epoc::Band::Gamma},
    }};

    for (const auto& [freq, band] : cases) {
        epoc::BandAnalyzer a(256);
        feed(a, {{freq, 1000.0}}, 256);
        a.compute();

        const double total = a.total_power(epoc::Channel::AF3);
        const double in_band = a.power(epoc::Channel::AF3, band);
        const double fraction = (total > 0.0) ? in_band / total : 0.0;
        tst::check(fraction > 0.95,
                   std::to_string(static_cast<int>(freq)) + " Hz lands in " +
                       std::string(epoc::band_name(band)));
    }
}

void test_superposition() {
    tst::section("superposition");

    // Two tones in different bands must each be measured correctly, which is
    // what makes a five-band readout meaningful rather than decorative.
    const double alpha_amp = 1000.0;
    const double beta_amp = 400.0;

    epoc::BandAnalyzer a(256);
    feed(a, {{10.0, alpha_amp}, {20.0, beta_amp}}, 256);
    a.compute();

    tst::check_near(a.power(epoc::Channel::AF3, epoc::Band::Alpha),
                    expected_power(alpha_amp), 0.03, "alpha component measured correctly");
    tst::check_near(a.power(epoc::Channel::AF3, epoc::Band::Beta),
                    expected_power(beta_amp), 0.03, "beta component measured correctly");

    // Total should be the sum of the two, with little elsewhere.
    tst::check_near(a.total_power(epoc::Channel::AF3),
                    expected_power(alpha_amp) + expected_power(beta_amp), 0.03,
                    "total power is the sum of the components");
}

void test_window_independence() {
    tst::section("window-length independence");

    // Band power is a physical quantity, so the answer must not depend on the
    // FFT length. This is the property the S2 normalisation buys, and it is
    // the one most likely to break if the scaling is ever touched.
    const double amplitude = 1000.0;
    const double want = expected_power(amplitude);

    for (const std::size_t window : {128u, 256u, 512u, 1024u}) {
        epoc::BandAnalyzer a(window);
        feed(a, {{10.0, amplitude}}, window);
        a.compute();
        tst::check_near(a.power(epoc::Channel::AF3, epoc::Band::Alpha), want, 0.05,
                        "window " + std::to_string(window) + " recovers the same power");
    }
}

}  // namespace

int main() {
    std::printf("epoc dsp tests\n\n");
    test_band_table();
    test_fft();
    test_analyzer_lifecycle();
    test_dc_rejection();
    test_absolute_calibration();
    test_band_assignment();
    test_superposition();
    test_window_independence();
    return tst::summary();
}
