# Status

Where the project stands as of **2026-09-18**, version `0.3.0`.

This is the "what can I trust" document. It separates what has been verified
against real hardware from what merely compiles, and lists the known open
items.

## Notes, Ideas & open features

- Rate limiting on OSC output exists in the viewer's analysis tool (a send
  rate, capped in the browser). The C++ side still has none.

## Feature state

| Area | State |
|---|---|
| Enumeration, interface probing | Verified on hardware |
| AES-128 decryption | Verified against FIPS-197 C.1 *and* live data |
| Key derivation | Verified on hardware (one dongle) |
| Channel decode, contact quality | Verified on hardware |
| Contact-quality refresh tracking (`quality_updated`) | Verified on hardware: 26.7% of packets carry a refresh, against 26.4% predicted |
| Gyro | Verified on hardware |
| Battery | Verified on hardware |
| Drop detection / sequence tracking | Verified on hardware (0 drops over long runs) |
| Live terminal display | Verified on Windows |
| Band powers (FFT) | Maths verified against synthetic signals; **not** cross-checked against a reference EEG implementation |
| OSC output | Verified on the wire, all message types and all four timestamp encodings |
| Wait-for-headset loop | Entry verified; the *wake-up transition* has not been observed |
| Linux build | **Never compiled** |
| macOS build | **Never compiled** |
| Viewer: OSC receive, HTTP/SSE, widgets | Verified end to end against a synthetic stream; **never run against a headset** |
| Viewer: analysis tool (valence/arousal, focus/relax) | Computes what the formulae say, against a synthetic stream; the indices themselves are **unvalidated against a person** |
| Viewer: OSC output | Verified on the wire — six addresses, float arguments, ~8 Hz sustained per stream from the browser |

## Hardware verification log

All observations from a single dongle, serial `SN201302043668GM`, on Windows
10 with MSVC.

**2026-09-17** — Streaming confirmed on interface 1: 127.9 Hz measured, 0
drops, battery 97%, counter sequencing cleanly through the battery frame.
Derived key `4d004754381036424d00474838003650`, which decrypts to sane channel
values — the end-to-end proof that the key layout is right.

**2026-09-17** — Feature-report probe: a real dongle answers with
`00 21 ff 1f ff 1e 00 00 00`. Separately established that
`hid_get_feature_report` does **not** gate streaming: all four permutations of
(probe / don't probe) × (interface 0 / 1) behaved identically, with interface 1
streaming 32-byte reports and interface 0 silent.

**2026-09-17** — OSC verified on the wire for all four timestamp encodings, by
decoding raw datagram bytes rather than trusting a receiver's display.

**2026-09-17** — Battery OSC message: exactly one `/epoc/battery` (level 0.85,
first seen 1.15 s into the stream) against 1448 `/epoc/gyro/x` in the same
~11 s window, confirming the change-only behaviour.

**2026-09-18** — Contact-quality OSC output, decoded off the wire over 1783
samples: `/epoc/raw/<channel>` 40 bytes with tags `,hii`; `/epoc/quality/all`
104 bytes with tags `,hiiiiiiiiiiiiii`, 476 messages — 26.7% of packets,
against the 34/129 = 26.4% predicted from the selector table. Values were low
(0..24) throughout, as expected for a headset sitting on a desk rather than on
a head; **the message has not been checked against electrodes in actual
contact**, so the >4000 good-contact figure remains emokit's claim rather
than something confirmed here.

## Open items

Nothing here is blocking; these are the known gaps, roughly in the order they
are worth addressing.

**Linux and macOS have never been built.** Every hook is in place — hidapi
backends, framework links, `Threads::Threads`, BSD sockets, `isatty`/
`TIOCGWINSZ`, the udev rule — but none of it has seen a compiler. Expect to
fix narrowing conversions: the GCC/Clang warning set includes `-Wconversion`
and `-Wsign-conversion`, which MSVC's `/W4` does not cover. Building the first
time with `-DXAVIER_WERROR=ON` is a good way to flush those out.

**`XAVIER_WERROR` is off by default** for exactly that reason. Turn it on once
a clean GCC/Clang build exists.

**The project licence is not chosen.** `LICENSE` contains a placeholder for
the user's own terms, plus the upstream emokit terms (its ISC-style per-file
notice and its public-domain/BSD-3 project licence) and a note on hidapi. The
upstream obligations are already discharged; only the new code's licence is
undecided.

**The version has not been bumped since `0.3.0`,** which predates the
`--osc-timestamp` work, the IPv4-preference fix and the battery message.

**Band powers have not been cross-checked against a reference implementation.**
The maths is pinned by tests that verify absolute calibration (a sinusoid of
amplitude A integrates to A²/2), band assignment, superposition and
independence from window length — so the normalisation is self-consistent and
physically correct. What has *not* happened is feeding the same EEG recording
through, say, MNE or EEGLAB and comparing. Worth doing before anyone treats
the numbers as clinically meaningful.

**The wake-up transition out of the wait loop is unobserved.** Entering the
loop is tested (`--index 0` reaches it deterministically); the headset waking
and the loop exiting has not been seen, because it needs a physical power
cycle at the right moment. It reuses the verified `probe_once()`.

**`test_epoc.cpp` predates `check.hpp`** and still carries its own local
`check()` helper and failure counter. The other two suites share `check.hpp`.
Harmless, but it should be unified.

**The viewer has never seen a headset.** `view/` was built and verified
entirely against `view/tools/fake_dat.py`, which reproduces the message set,
argument types, rates and timetag encoding over real UDP — so the decode
path, the transport and every widget are exercised, but by synthetic data.
The first run against hardware is the outstanding check.

**The viewer has no automated tests.** Verification so far is a synthetic
stream plus a scripted headless browser (`view/tools/cdp.py`); nothing would
catch a regression on its own. `view/viewer/osc.py` is pure and would be the
obvious first thing to pin, mirroring `test_osc.cpp` on the receive side.

**The viewer has only been opened in Chrome.** Nothing it uses is
Chrome-specific, but Firefox and Safari have not been tried.

**The analysis indices are unvalidated as psychology.** The formulae in
`view/doc/analysis.md` are implemented faithfully and produce the numbers
they claim to from known band powers, but whether a particular head's
valence reads positive when that person is happy has not been — and cannot
be — checked against `fake_dat.py`. Every threshold in the widget is a
display convention, which is why all of them are exposed as settings.

**The viewer's OSC output rate is bounded by the browser.** A widget posts
each result set to `/api/emit`, one request at a time, so the round trip is
part of every cycle: a 10 Hz setting delivered 7 to 8 Hz on the wire in
headless Chrome, with the analysis tool and the gyro both sending. Each
widget displays the rate it measures rather than the one it was set to.

## Repository state

The git history has one commit — the initial generated iteration — and
everything since is uncommitted:

- Modified: `CMakeLists.txt`, `README.md`, `epoc/CMakeLists.txt`,
  `epoc/cli/*`, `epoc/include/epoc/epoc.hpp`, `epoc/src/device.cpp`,
  `epoc/src/protocol.cpp`, `epoc/test/test_epoc.cpp`
- Untracked: `epoc/include/epoc/dsp.hpp`, `epoc/include/epoc/osc.hpp`,
  `epoc/src/dsp.cpp`, `epoc/src/osc.cpp`, `epoc/test/check.hpp`,
  `epoc/test/test_dsp.cpp`, `epoc/test/test_osc.cpp`, all of `view/`, and
  this documentation

So the working tree is substantially ahead of `HEAD`. Committing has not been
requested, and the branch is `master`.

## Test totals

162 assertions across three suites, all passing:

| Suite | CTest name | Assertions |
|---|---|---|
| `epoc_tests` | `epoc_protocol` | 41 |
| `epoc_dsp_tests` | `epoc_dsp` | 37 |
| `epoc_osc_tests` | `epoc_osc` | 84 |

See [testing](../epoc/doc/testing.md) for what they cover and, more usefully,
what they do not.
