# Testing

What the test suite covers, what it deliberately does not, and the conventions
to follow when adding to it.

```sh
ctest --test-dir build -C Release --output-on-failure
```

162 assertions across three suites, all currently passing, in about 3 seconds.

| Suite | CTest name | Source | Assertions |
|---|---|---|---|
| `epoc_tests` | `epoc_protocol` | `test/test_epoc.cpp` | 41 |
| `epoc_dsp_tests` | `epoc_dsp` | `test/test_dsp.cpp` | 37 |
| `epoc_osc_tests` | `epoc_osc` | `test/test_osc.cpp` | 84 |

## Why the tests matter more than usual here

The EPOC is discontinued. The reference implementation (emokit) was validated
only against live hardware, and there is no CI machine with a headset attached
to it. If the decode path regresses, the only way to find out would be to sit
down with the physical device — assuming one still works.

So the standing rule is: **anything that can be checked without hardware,
is.** The protocol layer is written as pure functions specifically to make
that possible.

## Framework

Deliberately none. `test/check.hpp` provides `tst::check`, `tst::check_near`,
`tst::failures()` and `tst::summary()` — about 40 lines. The tests must run
anywhere the library builds, with no extra dependency to fetch, on a project
whose whole point is being buildable on three platforms years after the
hardware was discontinued.

Each suite is a `main()` that runs its sections and returns
`tst::summary()`. Failures print both values for numeric comparisons, so a
regression is diagnosable from the CTest log alone without a debugger.

> `test_epoc.cpp` predates `check.hpp` and still carries its own local
> `check()` and failure counter. Harmless duplication; worth unifying.

## Coverage

### `epoc_protocol` — the decode path

| Section | Pins |
|---|---|
| AES-128 ECB | **FIPS-197 Appendix C.1** reference vector; determinism; in-place decrypt matches out-of-place (`device.cpp` relies on aliasing being safe) |
| key derivation | The **hardware-verified** vector `SN201302043668GM` → `4d004754381036424d00474838003650`; emokit's protocol-doc example `SN20120526998912` → `32003154391038423200314839003850`; only the last 4 characters matter; short serials key off their real tail (emokit's offset-16 bug); distinct serials give distinct keys; serials under 4 characters throw |
| bit unpacking | `mask[0]` is the LSB and `mask[13]` the MSB; packet byte 0 is never sample data; a saturated field is exactly 14 bits |
| battery | Table boundaries and the ≥248 saturation |
| packet decode | Counter vs battery frame; sticky battery and quality carry-in; gyro offsets; channel values |
| quality selectors | Which electrode a selector names (2 → AF3, the 64..80 mirror 80 → FC6); that an unmapped selector and a battery frame both leave `quality_updated` empty while the sticky values survive |
| channel table | Names, ordering, bounds |

The AES vector choice is load-bearing: validating against **FIPS-197** rather
than against a previous run of this code means the implementation is checked
against the standard, not against itself.

### `epoc_dsp` — spectral analysis

Covered in detail in [dsp.md](dsp.md#verification). In brief: absolute
calibration against the closed-form A²/2, band assignment, superposition,
independence from FFT window length, DC rejection, FFT primitives, analyzer
lifecycle.

### `epoc_osc` — encoding and option parsing

| Section | Pins |
|---|---|
| string padding | NUL termination and 4-byte alignment, including the "already a multiple of 4" case that needs a full 4 bytes of padding |
| argument encoding | Big-endian layout for every type; type tags follow argument order; a realistic epoch survives an int64 round-trip exactly |
| timestamp formats | All four encodings; **that a current epoch-ms int64 has a clear sign bit** but a negative low word — the truncation behaviour that motivated the option existing |
| message reuse | `reset()` clears arguments and type tags without leaking state between sends |
| realistic messages | Exact byte sizes for the messages actually emitted (raw 32, raw-with-quality 40, band 52, bundle 100, quality bundle 104, battery 32) and their type tag strings |
| message flags | Every flag; order independence; rejection of unknown and repeated flags; canonical formatting; format→parse round-trip; **that `b` is bands, `y` is battery and `q` is quality**, not each other |
| prefix normalisation | Leading slash, trailing slash, collapsed runs, the root case |
| endpoint parsing | `host:port`, `[ipv6]:port`, bare port, `:port`; rejection of bad ports and unterminated brackets |
| channel addresses | Lowercasing, all 14 |
| monotonic clock | Never goes backwards; returns plausible epoch milliseconds |

## Conventions

**Pin the bug, not just the fix.** When a defect is found, add an assertion
that fails on the old behaviour. The clearest example is the timestamp
sign-bit test: it asserts both that the int64 is positive *and* that its low
32 bits are negative, so the reason `--osc-timestamp` exists stays documented
in the test suite rather than only in a commit message.

**Choose test frequencies on exact bin centres.** At 0.5 Hz resolution, a tone
between bins spreads across a band boundary and the ≥95%-in-band thresholds
fail for reasons unrelated to correctness.

**Check exact byte counts for wire formats**, not just "it produced
something". OSC padding errors produce packets that look fine and are
unparseable.

**Verify the wire, not a GUI.** See
[osc.md](osc.md#the-negative-timestamp-non-bug) — trusting a third-party
receiver's rendering produced a bug report against correct code.

## What is not covered

Know these gaps before trusting a green run:

- **Everything behind hidapi.** `device.cpp` — enumeration, opening, reading,
  the two-interface probe — has no test. It needs either hardware or a fake
  HID layer, and the transport is currently not abstracted for injection.
- **The CLI.** Argument parsing, the display, and `OscStreamer` policy are
  untested. The *parsers* they call (`parse_message_flags`,
  `parse_endpoint`, `parse_timestamp_format`) are well covered, so the gap is
  the wiring, not the logic. `OscStreamer`'s send *cadence* is the largest
  piece of untested policy: that `/battery` fires on change and
  `/quality/all` on refresh has only ever been checked by counting datagrams
  off a live headset.
- **Linux and macOS.** Never compiled, let alone tested.
- **The wake-up transition** out of the wait-for-headset loop.
- **Band powers against a reference implementation** (MNE, EEGLAB).

The obvious next investment, if this becomes load-bearing, is a recorded
stream of encrypted 32-byte reports played back through a fake transport. That
would close the `device.cpp` gap and let the CLI loop be exercised end to end
without hardware. It needs `Device` to gain an injectable transport seam —
which the pImpl layout already makes straightforward.

## Adding a test

1. Put protocol/DSP/OSC assertions in the matching existing suite; a new CTest
   target is only warranted for a new subsystem.
2. Use `tst::check_near` for anything floating-point, with an explicit
   tolerance — and say in the message what the tolerance means.
3. Write the assertion message as a statement of the property
   (`"mask[0] is the LSB"`), not as a description of the action
   (`"test unpack"`). The messages are the readable spec of the wire format.
