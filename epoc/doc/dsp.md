# Spectral analysis

How band powers are computed, why the normalisation is what it is, and how it
is verified. Implementation: [`src/dsp.cpp`](../src/dsp.cpp), header
[`include/epoc/dsp.hpp`](../include/epoc/dsp.hpp).

## Bands

| Band | Range (Hz) |
|---|---|
| delta | 0.5 – 4 |
| theta | 4 – 8 |
| alpha | 8 – 13 |
| beta | 13 – 30 |
| gamma | 30 – 45 |

Ranges are half-open `[low, high)`, so a bin exactly on a boundary belongs to
the upper band and no bin is counted twice.

These are the conventional clinical edges. The one judgement call is **gamma
stopping at 45 Hz** rather than at the 64 Hz Nyquist limit: the EPOC's analogue
front end band-limits to roughly 0.16–43 Hz, so above 45 Hz there is nothing
but filter roll-off and mains hum. Extending gamma upward would produce a
number that moves — it would just be measuring the wrong thing.

## Pipeline

Per channel, once per `compute()`:

1. **Ring buffer** of the last `window` samples, stored in microvolts
   (counts × `kMicrovoltsPerCount`).
2. **Subtract the mean** over the window.
3. **Hann window.**
4. **FFT** — in-place iterative radix-2 Cooley–Tukey.
5. **One-sided PSD**, then integrate over each band.

### Why the mean is removed

EPOC counts sit on a large DC offset — around 8400 counts, about 4.3 mV. Left
in, it dominates every real rhythm by orders of magnitude, and the Hann
window's spectral leakage smears it straight into delta, which is adjacent to
DC. Removing the mean per window (not a fixed constant, since the offset
drifts) is what makes delta readable at all.

Bin 0 (DC) and bin N/2 (Nyquist) are additionally skipped during integration:
DC has already been subtracted, and Nyquist is far above the device's analogue
bandwidth.

### Windowing and normalisation

The Hann window suppresses the leakage a rectangular window would smear across
every band. But any window changes the amplitude of the result, so it has to
be normalised out.

```cpp
const double scale = 2.0 / (sample_rate_ * window_power_sum_);   // window_power_sum_ = S2 = sum(w^2)
...
const double psd = scale * std::norm(scratch_[k]);               // uV^2/Hz
out[b] += psd * df;                                              // uV^2
```

So the one-sided power spectral density is

```
PSD_k = 2 · |X_k|² / (fs · S2)      with S2 = Σ w[i]²
```

and band power is that integrated over the band — multiplied by the bin width
`df = fs / N` and summed.

Two parts of this are load-bearing:

- **The factor 2** makes it one-sided: the negative-frequency half of the
  spectrum is folded onto the positive half rather than discarded.
- **Dividing by S2 rather than by N (or by (Σw)²)** is what makes the
  integrated power independent of the window *shape* and *length*. Get this
  wrong and the numbers still look plausible — they simply change when you
  pass `--window 512`. This is the property most likely to break if anyone
  touches the scaling, so there is a test dedicated to it.

Result units are **µV²**. The CLI's `--relative` mode instead shows each band
as a percentage of the channel's total, which sidesteps the calibration
question entirely and is often the more useful view.

## Window length

`--window N`, a power of two, 64 ≤ N ≤ 4096; default 256.

| Window | Duration @128 Hz | Bin width |
|---|---|---|
| 128 | 1.0 s | 1.0 Hz |
| **256** | **2.0 s** | **0.5 Hz** |
| 512 | 4.0 s | 0.25 Hz |
| 1024 | 8.0 s | 0.125 Hz |

256 is the default because 0.5 Hz bins are fine enough to separate delta
(starting at 0.5 Hz) from theta while keeping the display responsive. Longer
windows resolve the low bands better and react more slowly — the trade-off is
inherent, not an implementation limit.

`power()` returns zero until a full window has been collected; `ready()` and
`filled()` let the CLI show a countdown instead of a table of zeros.

## Compute cadence

`push()` is called at the full 128 Hz; `compute()` is called once per display
refresh (default 50 ms). That separation is deliberate: 14 FFTs at 128 Hz
would be ~1800 transforms a second, and nobody would see the difference.

A consequence worth knowing when consuming the OSC band messages: `/fft/*`
messages are emitted at the *refresh* rate, not the sample rate, and only once
the window has filled.

## Verification

The maths is pinned by `test_dsp.cpp` against synthetic signals, which is the
only way to check absolute correctness — real EEG has no known answer.

| Test | What it pins |
|---|---|
| **absolute calibration** | A 10 Hz sinusoid of amplitude A integrates to **A²/2** in alpha, within 3%. This is the closed-form answer for a sinusoid's power, so it validates the entire scaling chain at once. |
| | ≥95% of the total power lands in alpha, not smeared into neighbours. |
| | All 14 channels produce identical results for identical input. |
| **band assignment** | One tone per band (2, 6, 10, 20, 38 Hz), each ≥95% in its own band. |
| **superposition** | Two tones (10 Hz @1000, 20 Hz @400) are *each* measured correctly, and the total is their sum. This is what makes a five-band readout meaningful rather than decorative. |
| **window independence** | The same signal recovers the same power at windows 128, 256, 512 and 1024, within 5%. This is the S2 normalisation property described above. |
| **DC rejection** | Constant input yields essentially zero band power (< 1e-6). |
| **FFT** | Impulse → flat spectrum; known sinusoid → energy in the expected bin; round-trip properties. |
| **analyzer lifecycle** | `ready()`/`filled()` transitions, zero output before the window fills, constructor rejects non-power-of-two and too-small windows. |

Test frequencies are chosen to fall on **exact bin centres** at 0.5 Hz
resolution, so the Hann main lobe stays entirely within one band. A tone
between bins would spread across the boundary and the ≥95% thresholds would
fail for reasons that have nothing to do with correctness. If you add a test
frequency, pick a multiple of the bin width.

### What is *not* verified

The output has never been compared against a reference EEG implementation
(MNE, EEGLAB, or similar) on the same recording. Everything above establishes
that the numbers are *self-consistent and physically correct for signals with
a known closed-form answer*. That is a strong claim, but it is not the same as
agreeing with an established toolchain on real data — notably around the
conventions those tools use for detrending and windowing. Worth doing before
anyone treats these numbers as clinically meaningful.
