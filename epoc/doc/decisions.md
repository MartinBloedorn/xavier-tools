# Design decisions

A log of the decisions that shaped this port, each with the reasoning and —
where it exists — the evidence. The value here is mostly in the **rejected**
alternatives: several look obviously right until you know why they were
dropped.

---

<a id="adr-1"></a>

## ADR 1 — C++17, CMake, library + thin CLI

emokit is C with a hand-rolled build. The port targets C++17 with standard
CMake, and splits into a static library (`libepoc`) plus an executable that
only parses arguments and renders.

The split is what makes the decode path testable and what will let a second
tool (or a GUI) reuse the device layer without dragging in terminal handling.

**Rejected:** keeping it a single executable. Cheaper today, but it would put
the protocol behind `main()` where nothing can reach it.

---

<a id="adr-2"></a>

## ADR 2 — hidapi via FetchContent, pinned, with a system fallback

`cmake/GetHIDAPI.cmake` tries an installed package, then pkg-config, then
`FetchContent` pinned to `hidapi-0.14.0` built static.

hidapi abstracts exactly the three backends this project needs (Windows HID
API, Linux hidraw, macOS IOKit) and nothing else. Pinning keeps a fresh clone
building years from now; the system-first order keeps Linux distributions
happy.

**Rejected:** libusb directly. It would mean writing three transport backends
by hand for no gain — and on Windows, fighting the HID class driver for a
device that is perfectly well-behaved as a HID.

---

<a id="adr-3"></a>

## ADR 3 — Own AES-128, generated tables, FIPS-validated

emokit depends on libmcrypt, which is unmaintained and a genuine porting
obstacle on Windows. It is replaced by ~160 lines of decrypt-only AES-128 in
`src/aes128.cpp`.

Two choices inside that:

**The S-box is generated algorithmically** (GF(2⁸) multiplicative inverse plus
the affine transform) in a function-local static, rather than transcribed.
Transcribing 512 bytes of lookup table is the single likeliest way to get AES
subtly wrong — and a subtly wrong AES produces plausible-looking garbage, not
an obvious failure.

**It is validated against the FIPS-197 Appendix C.1 vector**, not against a
previous run of this code. `Device`'s constructor runs the self-test and
refuses to open if it fails, so a broken build cannot silently hand back
garbage samples.

---

<a id="adr-4"></a>

## ADR 4 — One key layout, not two

emokit carried two key layouts ("consumer" and "research"), selected by
comparing a HID feature report against a fixed pattern. Only one is kept.

**This one was reversed once and then reinstated**, so the evidence matters.
After the removal, a run failed with "found the dongle but no interface is
streaming", and the removed layout was the obvious suspect. It was not the
cause. What established that:

1. **A real dongle answers the feature report with
   `00 21 ff 1f ff 1e 00 00 00`**, which does not match emokit's "consumer"
   pattern — so the probe selects the kept layout anyway. The other branch is
   unreachable on this hardware.
2. **emokit's own Python port hardcodes this layout** and ignores the probe
   entirely, with a comment doubting the other branch was ever useful.
3. **emokit's `doc/emotiv_protocol.asciidoc` specifies this layout.** The doc
   and the code disagreed *only* about the removed one.
4. **Decryption happens after the read.** A wrong key cannot produce silence —
   it produces a live 128 Hz table of garbage. The observed symptom was
   therefore structurally incompatible with a key problem.
5. Re-running the binary confirmed interface 1 streaming at 127.9 Hz, 0 drops,
   battery 97%.

The actual cause was a headset that had gone to sleep — which led directly to
[ADR 6](#adr-6).

If a dongle ever does need the other arrangement, it is the `(n-1, n-2)` and
`(n-3, n-4)` pairs at key offsets 4..10 that move; the git history of
`protocol.cpp` has the original.

---

<a id="adr-5"></a>

## ADR 5 — Identify the streaming interface by listening

The dongle presents two HID interfaces; only interface 1 delivers EEG, and
interface 0 opens cleanly and stays silent forever. There is no flag, no
descriptor field and no error that distinguishes them.

So `probe_once()` opens each candidate and waits for an actual packet, with
interface 1 tried first. This is the only reliable method, and it explains
emokit's otherwise unexplained "try index 1, then 0".

`--index`/`--path` pin the candidate list to one interface but deliberately do
**not** bypass the probe or the wait: choosing *which* device to use should not
also mean "and don't wait for it". That refactor (into `resolve_candidates()`)
also made the wait loop testable, since `--index 0` is a reliably silent device.

---

<a id="adr-6"></a>

## ADR 6 — Wait for the headset by default

The headset sleeps on its own. "Dongle present but silent" is therefore a
routine, recoverable state, not an error — so by default the tool keeps
listening, re-enumerating each pass (so unplug/replug is picked up) with a
400 ms per-interface timeout to stay responsive to Ctrl+C.

This means `epoc` can be started before the headset is switched on, in either
order. A *missing dongle* is different — that is a setup problem waiting will
not fix — so it still fails immediately. `--no-wait` restores the old
behaviour.

---

<a id="adr-7"></a>

## ADR 7 — Band power normalised by S2, in µV²

`PSD = 2·|X|²/(fs·S2)` with `S2 = Σw²`, integrated over each band. See
[dsp.md](dsp.md#windowing-and-normalisation).

Normalising by the window's power sum rather than by N is what makes the
result independent of window shape and length. Every other choice produces
numbers that look plausible and change when you pass `--window 512`.

The tests pin the physics, not the implementation: a sinusoid of amplitude A
must integrate to A²/2, and the same signal must give the same answer at four
different FFT lengths.

---

<a id="adr-8"></a>

## ADR 8 — Hand-rolled OSC, not a library

`osc.cpp` implements only what this tool sends: five argument types, no
bundles, no pattern matching, no receiving. About 200 lines of encoding.

liblo or oscpack would have been another dependency, another platform build to
verify, and another thing to pin — to avoid writing code whose whole
specification is "strings are NUL-padded to 4 bytes and numbers are
big-endian". The test suite covers the encoding at byte level.

---

<a id="adr-9"></a>

## ADR 9 — Selectable timestamp encoding

Prompted by a report of negative int64 timestamps in Hexler's Protokol. The
wire bytes were correct and positive; Protokol truncates the int64 to 32 bits
and displays the low word. Full analysis in
[osc.md](osc.md#the-negative-timestamp-non-bug).

The decision worth recording is what happened *after* the diagnosis. "The
encoding is correct per spec" was true and useless: the user still could not
verify their stream. And since OSC 1.0 only *requires* `i`/`f`/`s`/`b`, the
problem generalises to other receivers.

So `--osc-timestamp` offers `int64`, `double`, `int32` and `timetag`.

**float32 was deliberately excluded**: 24 bits of mantissa quantises a
millisecond epoch to steps of about four minutes. A test asserts this.

---

<a id="adr-10"></a>

## ADR 10 — Prefer IPv4 when resolving

Found while reproducing the timestamp report: `--osc localhost:PORT` resolved
to `::1` on a dual-stack Windows machine, and an IPv4-only receiver got
nothing, with no error anywhere — UDP reports nothing when a datagram has no
listener.

Resolution now tries `AF_INET` first and falls back to `AF_UNSPEC`. IPv6 stays
reachable via a bracketed literal (`--osc [::1]:9000`), and the banner reports
the resolved numeric address so this failure mode is visible rather than
silent.

---

<a id="adr-11"></a>

## ADR 11 — Battery on change only, flagged `y`

`<prefix>/battery` carries a 0..1 float, sent when the first reading arrives
and thereafter only when the value changes.

**Why not at literal startup**, as originally requested: `Frame::battery` is
sticky and reads 0 until the first battery frame arrives, roughly a second
into the stream. Sending at startup would announce a flat battery that isn't.
The trigger is `is_battery_frame && value != last`, which yields the first
*real* value within a second — measured at 1.15 s.

**Why `y` and not `b`:** `b` was already band powers.

**Why no periodic resend:** a receiver started after `epoc` will not see the
level until it next changes, which is a genuine limitation of change-only
semantics over UDP. The requested behaviour was change-only, so that is what
was built, and the limitation is documented in the README rather than
silently designed around.

---

<a id="adr-12"></a>

## ADR 12 — Warnings via an interface target; `WERROR` off

`xavier_warnings` is an INTERFACE target linked by our targets only, so fetched
third-party code (hidapi) is not held to our warning level — which would
otherwise make `XAVIER_WERROR=ON` fail on code we do not own.

`XAVIER_WERROR` defaults **off** because the GCC/Clang set includes
`-Wconversion` and `-Wsign-conversion`, which have never been exercised: the
project has only been compiled with MSVC. Turning it on during the first Linux
build is the recommended way to flush out the narrowing conversions, once
somebody is in a position to fix them.

---

<a id="adr-13"></a>

## ADR 13 — Contact quality as a modifier, `q`, off by default

`q` does not add a message of its own. It appends a quality argument to each
`<prefix>/raw/<channel>` and, alongside `u`, emits `<prefix>/quality/all`.
With neither `r` nor `u` it sends nothing.

**Why two shapes rather than one.** They answer different questions. The
argument on `/raw/<channel>` answers *is this sample trustworthy*, and that
has to ride along with the sample — a receiver should not have to correlate
two streams to decide whether to believe a number. `/quality/all` answers
*how is the cap sitting*, which is one question about fourteen electrodes and
belongs in one message.

**Why it is not in the default flags**, unlike `r`, `b`, `g` and `y`: it
changes the arity of an existing message. A receiver parsing
`/raw/af3 <ts> <count>` positionally keeps working today and would break on
an upgrade if `q` were default. It is the only flag whose effect is not
purely additive, so it is the only one that is opt-in.

**Why `/quality/all` is gated on a refresh, not on the sample clock.** A
report carries quality for exactly one electrode, and only 34 of the 129
counter states name one at all, so sending per sample would repeat each
reading nearly four times. This needed a new field, `Frame::quality_updated`:
`quality` is sticky, so freshness is not observable from it, and comparing
against the previous frame is not the same test — a refresh returning an
unchanged value is still new information.

**Rejected: change-only, like the battery.** It would be quieter, but quality
is what tells an operator whether the data is worth recording, and a change-
only stream leaves a receiver that joined late with nothing. At ~34 messages
a second — against 1800 for `r` — the full set costs nothing and converges
within half a second. Each message therefore carries all 14 values, not just
the one that was refreshed.

**Why `q` and not `c`:** nothing else claimed either letter; `q` reads as
quality, and `c` would invite confusion with *channel*.
