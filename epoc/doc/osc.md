# OSC output

Design and implementation notes for the OSC streaming path. User-facing usage
is in the [README](../../README.md#osc-output); this covers *why* it works the
way it does, and the interoperability traps.

Implementation: [`src/osc.cpp`](../src/osc.cpp),
[`include/epoc/osc.hpp`](../include/epoc/osc.hpp), and the `OscStreamer` class
in [`cli/main.cpp`](../cli/main.cpp).

## Split of responsibilities

| Where | Owns |
|---|---|
| `osc.cpp` / `osc.hpp` | **Encoding**: byte layout, padding, endianness, the UDP socket, option *parsing* helpers |
| `OscStreamer` in `main.cpp` | **Policy**: which messages exist, when they are sent, what their addresses are |

The split matters when adding a message type. Address construction and send
cadence are policy and belong in `OscStreamer`; anything that touches the
argument encoding belongs in `Message`. `osc.hpp` deliberately knows nothing
about EEG beyond `address_channel(Channel)`.

Everything parseable is parsed **before the hardware is touched** — a typo in
`--osc-messages` or an unresolvable host must fail immediately rather than
after a device probe, or worse, after sitting in the wait-for-headset loop.
That ordering in `run()` is deliberate; do not move OSC setup below
`open_streaming()`.

## Messages

| Address | Arguments | Rate |
|---|---|---|
| `<prefix>/raw/<channel>` | `<ts> <count>`, `<ts> <count> <quality>` with `q` | per sample, 128 Hz |
| `<prefix>/raw/all` | `<ts> <af3> <f7> …` (14) | per sample, 128 Hz |
| `<prefix>/quality/all` | `<ts> <af3> <f7> …` (14) | on refresh, ≈34 Hz |
| `<prefix>/fft/<channel>` | `<ts> <delta> <theta> <alpha> <beta> <gamma>` | per spectrum (≈20 Hz) |
| `<prefix>/gyro/x`, `<prefix>/gyro/y` | `<ts> <value>` | per sample, 128 Hz |
| `<prefix>/battery` | `<ts> <level>` | on change, and every 5 s |

Flags, selected by `--osc-messages`, canonical order `tbrugyq`, default
`tbrgy`:

| Flag | Message |
|---|---|
| `t` | prepend the timestamp to every message |
| `b` | band powers |
| `r` | per-channel raw |
| `u` | all raw channels in one message |
| `g` | gyro |
| `y` | battery |
| `q` | contact quality (a modifier on `r` and `u`, not a message of its own) |

**Battery is `y`, not `b`** — `b` was already band powers. Two tests pin this
specifically, since wiring `b` to both is the obvious slip. `q` gets a third
such test for the same reason.

Addresses are precomputed once in the `OscStreamer` constructor, not built per
message: with `r` enabled this loop runs about 1800 times a second, and
rebuilding strings there is pure waste. The `Message` object is likewise
reused across sends, with `reset()` clearing it without deallocating.

Channel names are **lowercased** in addresses (`/raw/af3`, not `/raw/AF3`).
OSC address patterns are case-sensitive, so this is fixed independently of the
display casing.

The two gyro axes get separate addresses rather than one two-argument message
so a receiver can route each independently.

### Battery semantics

Sent when the first reading arrives, thereafter when the value changes, and
every `kBatteryResend` (5 s) regardless. The trigger is:

```cpp
if (!flags_.battery || !f.is_battery_frame) return;
if (level == last_battery_ && now - last_battery_sent_ < kBatteryResend) return;
```

Gating on `is_battery_frame` rather than on the value changing is deliberate.
`Frame::battery` is **sticky** and reads 0 until the first battery frame
arrives, so a plain change test would announce a fictitious flat battery at
startup. The dongle substitutes a reading for the sequence counter once a
second, so the first real value follows the start of the stream within about a
second — measured at 1.15 s.

The resend is gated on that same flag, so it inherits the guarantee: nothing
is sent before a level has actually been measured. That also makes the
interval a floor rather than a period — the resend lands on the first battery
frame at or after 5 s, so the real gap is 5–6 s. Battery moves in 13 coarse
steps ([protocol](protocol.md)), so nothing is lost to that slack. The timer
reads `steady_clock`, not the OSC timestamp, so the cadence does not change
with `--osc-timestamp`.

The resend is what makes `/battery` useful to a receiver that joins
mid-stream: under change-only semantics it saw nothing until the charge next
moved, which on a healthy headset could be hours. See
[decisions](decisions.md#adr-11).

### Contact quality semantics

`q` is the only flag that is a **modifier rather than a message**. It does two
different things depending on what it is paired with, and nothing at all on
its own:

| Paired with | Effect |
|---|---|
| `r` | a second `i` argument on every `<prefix>/raw/<channel>`: `,hii` instead of `,hi` |
| `u` | a new message, `<prefix>/quality/all`, 14 values in channel order |
| neither | nothing — `sends_anything()` deliberately does not count `q` |

The two halves answer different questions, which is why both exist. The
argument on `/raw/<channel>` answers *is this particular sample trustworthy*,
and must therefore travel with the sample rather than on a second stream the
receiver would have to join. `/quality/all` answers *how is the headset
sitting*, which is a whole-cap question and belongs in one message.

**The rates differ, and that is the interesting part.** The per-sample
argument is the sticky last-known value for that electrode, repeated at
128 Hz. `/quality/all` is sent only when a report actually refreshes a
reading:

```cpp
if (!flags_.quality || !flags_.raw_bundle || !f.quality_updated) return;
```

A report carries quality for exactly one electrode, named by byte 0, and only
34 of the 129 counter states name one at all — so a fresh reading exists about
34 times a second and sending per sample would repeat each one nearly four
times. Measured on hardware: 476 `/quality/all` against 1783 samples, i.e.
26.7% of packets, against a predicted 34/129 = 26.4% — the excess is
boundary effect, since the run neither starts nor ends on a cycle.

This is what `Frame::quality_updated` exists for. `Frame::quality` is sticky
and cannot be tested for freshness, and comparing the array against the
previous frame is not the same thing — a refresh that happens to return the
same value is still new information. See
[protocol.md](protocol.md#contact-quality).

Every message carries all 14 values rather than just the refreshed one. That
costs nothing at this rate and means a receiver that joins mid-stream — or
loses a datagram, which UDP will not tell it about — converges within about
half a second instead of waiting for the next change.

`q` is **not in the default flags**, unlike every other message-bearing flag.
Enabling it changes the arity of an existing message, so making it default
would silently break receivers parsing `/raw/<channel>` positionally. See
[decisions](decisions.md#adr-13).

## Encoding

Hand-rolled OSC 1.0, no library. Only what this tool sends is implemented: no
bundles, no pattern matching, no receiving.

Two rules govern the byte layout, and both are easy to get wrong:

**Strings are NUL-terminated and padded to a multiple of four**, with *always
at least one* terminator:

```cpp
const std::size_t padded = (s.size() / 4 + 1) * 4;
```

A 4-character address therefore occupies 8 bytes, not 4. (This cost one wrong
test expectation during development: `/two` + the `,` type-tag string is 12
bytes, not 8.)

**All numeric arguments are big-endian**, regardless of host byte order.

Supported argument types: `i` (int32), `h` (int64), `f` (float32), `d`
(double), `t` (time tag).

## Timestamp encoding

This is the part with the real interoperability story.

**OSC 1.0 only *requires* a receiver to support `i`, `f`, `s` and `b`.**
`h` (int64), `d` (double) and `t` (time tag) are all optional extensions. Which
one a given receiver renders correctly is a property of that receiver, not of
the protocol.

`--osc-timestamp` selects:

| Value | Type | Meaning |
|---|---|---|
| `int64` (default) | `h` | epoch ms — exact, not universally supported |
| `double` | `d` | epoch ms — exact (53-bit mantissa ≫ the 41 bits needed), wider support |
| `int32` | `i` | ms since stream start — always supported, relative, wraps after ~24 days |
| `timetag` | `t` | NTP time tag — the canonical OSC time type |

Each also accepts its type letter as an alias (`h`, `d`, `i`, `t`).

**float32 is deliberately not offered.** With 24 bits of mantissa it cannot
represent a millisecond epoch at all — it quantises to steps of roughly four
minutes. There is a test asserting this reasoning stays documented.

### The negative-timestamp non-bug

Reported symptom: Hexler's Protokol showed a stream of **negative** int64
timestamps. It was not a bug in this code. Raw wire bytes:

```
timestamp bytes : 00 00 01 a0 b0 d7 44 3d
  big-endian int64 : 1789673292861   ← positive, decodes to the correct UTC time
  sign bit set?    : no
  LOW 32 bits as int32 : -1328069571 ← exactly what Protokol displayed
```

Protokol renders the int64 through a 32-bit path and shows the low half. Every
current epoch-millisecond value has a low word above 2³¹, which is why *every*
timestamp looked negative rather than just some.

Two lessons worth keeping:

1. **Decode the bytes, don't trust a receiver's display.** A third-party tool's
   rendering is evidence about that tool, not about the wire.
2. **"Correct per spec" is not a resolution** when the user cannot verify
   anything. The fix was to make the encoding selectable, not to declare the
   code right and stop.

If a receiver shows negative or nonsensical timestamps: try
`--osc-timestamp double`, then `int32`.

### Monotonic clock

`MonotonicUnixClock` anchors to `system_clock` **once** at construction and
thereafter advances by a `steady_clock` delta:

```cpp
return base_unix_ms_ + (now_ns - base_steady_ns_) / 1000000;
```

So values are real UNIX epoch milliseconds — they carry calendar meaning — but
cannot jump backwards or repeat if NTP steps the system clock mid-recording.
Using `system_clock` directly would have been simpler and wrong: a mid-session
NTP correction would corrupt the time base of a recording in a way that is
very hard to detect afterwards.

One timestamp is taken **per sample** and shared by every message describing
that sample, so a receiver can correlate `/raw/af3` with `/gyro/x` by exact
equality rather than by proximity.

`unix_ms_to_timetag()` converts to NTP: epoch 1900 (offset 2208988800 s) in
the high word, 2⁻³² second units in the low word.

## Networking

UDP, connectionless, best-effort. Failed sends are counted and surfaced in the
live display and the exit summary, never thrown — a dropped datagram must not
take down a live acquisition.

### IPv4 preference

`getaddrinfo` is called with `AF_INET` first, falling back to `AF_UNSPEC`:

```cpp
hints.ai_family = AF_INET;
int rc = getaddrinfo(host.c_str(), port_text.c_str(), &hints, &results);
if (rc != 0 || !results) {
    hints.ai_family = AF_UNSPEC;
    rc = getaddrinfo(host.c_str(), port_text.c_str(), &hints, &results);
}
```

This exists because of a real failure: on a dual-stack Windows machine
`localhost` resolves to `::1` before `127.0.0.1`, so `--osc localhost:9000`
sent every packet into the IPv6 loopback. Most OSC receivers bind IPv4 only,
and UDP reports nothing when a datagram has no listener — the symptom is
perfect silence with no error anywhere.

IPv6 is still reachable deliberately, via a bracketed literal:
`--osc [::1]:9000`.

The resolved numeric address is recorded with `getnameinfo(..., NI_NUMERICHOST)`
and printed in the startup banner, so this class of problem is visible rather
than silent:

```
epoc: streaming OSC to localhost:9000 (127.0.0.1), prefix /epoc, messages [tbrgy], timestamp int64
```

When testing resolution behaviour, bind the test receiver to `127.0.0.1`
specifically — a receiver on `0.0.0.0` accepts the IPv6 traffic too and hides
the bug.

### Endpoint syntax

`parse_endpoint()` accepts:

- `host:port`
- `[ipv6]:port`
- `:port` — host defaults to `127.0.0.1`
- `port` — a bare number, host defaults to `127.0.0.1`, since a local receiver
  is the overwhelmingly common case

Host/port splitting uses `rfind(':')` so IPv6 literals are unambiguous only in
bracketed form, which is the standard convention.

### Prefix normalisation

`normalize_prefix()` guarantees exactly one leading `/`, drops a trailing `/`,
and collapses repeated slashes. An empty or all-slash prefix yields `/`, and
address joining special-cases that root so you get `/raw/af3` rather than
`//raw/af3`.

## Message rates

| Flags | Messages/second |
|---|---|
| `r` | ~1800 (14 per sample) |
| `u` | 128 |
| `q` with `u` | ~34 |
| `q` with `r` | 0 extra messages, 4 extra bytes each |
| `g` | 256 |
| `b` | ~280 |
| `y` | ~0 |

If a receiver struggles, `u` carries the same data as `r` in a fourteenth of
the packets.
