# EPOC wire protocol

Everything known about how the Emotiv EPOC dongle talks, including the parts
that are undocumented and were established empirically. This is the most
expensive knowledge in the repository to re-derive — the headset is
discontinued and the original reference implementation (emokit) was validated
only against live hardware.

Implementation: [`src/protocol.cpp`](../src/protocol.cpp),
[`src/device.cpp`](../src/device.cpp), [`src/aes128.cpp`](../src/aes128.cpp).

## USB and HID

The dongle is USB `21a1:0001` (vendor `0x21a1`, product `0x0001`), shared by
all Emotiv receivers. It appears as a HID device and delivers fixed **32-byte
input reports** at a nominal **128 Hz** (measured 127.9 Hz).

Reports carry **no report id**, and hidapi already normalises away the leading
zero byte that the Windows HID API prepends. A 32-byte read is correct on all
three platforms — do not add a platform-conditional offset.

### The two interfaces

**The dongle presents two HID interfaces, and only interface 1 streams EEG.**

This is the single most important undocumented fact about the device. Both
interfaces:

- enumerate normally, with the same serial number,
- open successfully with `hid_open_path()`,
- report no error of any kind.

Interface 0 then simply never delivers a report. Not slowly — never. There is
no error, no short read, no status bit; it is indistinguishable from a working
interface until you wait and nothing arrives.

Consequences baked into the code:

- `streaming_candidates()` stable-sorts interface 1 first, preserving bus order
  otherwise, so multiple dongles stay predictably ordered.
- The only way to identify the right interface is **to listen**. `probe_once()`
  opens each candidate and waits `--settle` (default 2000 ms) for a first
  packet. This is why the tool prints `probing interface 1 ...` at startup.
- This explains the otherwise unexplained "try index 1, then 0" in emokit.

A useful side effect for testing: interface 0 is a *reliably silent* device.
`epoc --index 0` is the deterministic way to exercise the wait-for-headset
path with no special hardware state.

### The feature report is a red herring

emokit issued a HID feature report and compared the answer against a fixed
pattern to choose between two key layouts. Two things were established here:

1. A real dongle answers with `00 21 ff 1f ff 1e 00 00 00`.
2. **The feature report does not gate streaming.** All four permutations of
   (issue the probe / skip it) × (interface 0 / interface 1) behave
   identically: interface 1 streams, interface 0 does not. The probe is not a
   wake-up call or an enable command.

The tool therefore does not issue it at all.

## Encryption

Each 32-byte report is **AES-128 in ECB mode**, as two *independent* 16-byte
blocks. There is no chaining, no IV, and no padding to worry about — decrypt
bytes 0..15 and 16..31 separately with the same key.

The key is derived from the dongle's serial number, which is read over HID
(`hid_get_serial_number_string`). Only the **last four characters** participate.

Writing `c(1)` for the last character, `c(2)` for the second-to-last, and so
on, the key is:

```
c(1), 0x00, c(2), 'T',
c(3), 0x10, c(4), 'B',
c(1), 0x00, c(2), 'H',
c(3), 0x00, c(4), 'P'
```

Hardware-verified vector:

```
serial SN201302043668GM  ->  4d 00 47 54 38 10 36 42 4d 00 47 48 38 00 36 50
                             M     G   T  8     6  B  M     G  H  8     6  P
```

A second vector, the worked example from emokit's own
`doc/emotiv_protocol.asciidoc`:

```
serial SN20120526998912  ->  32003154391038423200314839003850
```

Both are asserted in `test_epoc.cpp`. The first is the canonical one: it is
confirmed to decrypt a real stream.

Two deviations from emokit, both deliberate:

- **Indexing from the end.** emokit indexed the serial at a hardcoded offset
  of 16, which silently produced a wrong key for any serial that was not
  exactly 16 characters. `derive_key()` indexes from the actual end and throws
  if the serial is shorter than 4 characters.
- **One layout, not two.** See [decisions](decisions.md#adr-4) for the full
  evidence trail; the short version is that emokit's second ("consumer")
  layout is unreachable on real hardware, its own Python port hardcoded this
  one, and this is the layout that emokit's protocol document specifies.

### A wrong key does not look like silence

Worth internalising, because it has already caused one misdiagnosis:
decryption happens *after* the read. If the key were wrong you would get a
live, updating table of garbage at the full 128 Hz — not an absence of data. A
tool that reports "no interface is streaming" has a transport or headset
problem, never a key problem.

## Report layout

After decryption, the 32 bytes are:

| Byte | Contents |
|---|---|
| 0 | Sequence counter, **or** battery level, **or** contact-quality selector — all three at once, see below |
| 1..28 | 14 channels × 14 bits, bit-interleaved (see the mask table) |
| 13..15 | *also* the 14-bit contact-quality value — overlapping the channel region |
| 29 | Gyro X, biased |
| 30 | Gyro Y, biased |
| 31 | Not used by this decoder |

### Byte 0 is overloaded three ways

```cpp
if (packet[0] & 0x80) {           // high bit set
    counter = 128;                //   sentinel: this was not a counter
    is_battery_frame = true;
    battery = battery_percent(packet[0]);
} else {
    counter = packet[0];          // 0..127
}
```

- **As a counter** it runs 0..127 and wraps.
- **Once per second** the dongle overwrites it with a battery reading, flagged
  by the high bit. The frame is otherwise a normal EEG sample — the channel
  data is still valid.
- **As a quality selector** it also names which electrode this report's
  contact-quality reading refers to.

Because a battery frame's byte 0 is ≥ 128 and the selector table only covers
0..16 and 64..80, battery frames update no contact quality. That falls out of
the table rather than being special-cased.

Sequence tracking therefore runs over **129 values**: 0..127 plus the
battery frame represented as 128. `DropCounter` in the CLI models exactly
that; a naive mod-128 sequence check would report a phantom drop every second.

### Channel bit unpacking

Each channel's 14-bit sample is scattered across the report. `kChannelMasks`
gives, for each channel, the 14 bit positions that make it up:

```cpp
int unpack_level(const std::uint8_t packet[32], const std::uint8_t bits[14]) {
    int level = 0;
    for (int i = 13; i >= 0; --i) {
        level <<= 1;
        level |= (packet[bits[i] / 8 + 1] >> (bits[i] % 8)) & 1;
    }
    return level;
}
```

Two things it is easy to get backwards, both of which are pinned by tests:

- **`bits[0]` is the least significant bit, `bits[13]` the most.** Trace the
  loop: `bits[13]` is consumed first and then shifted left 13 more times.
- **Bit positions are counted from packet byte 1**, not byte 0 — hence the
  `+ 1`. Byte 0 is the counter, and is not part of any channel.

Measured layout facts (derived from the mask table, not from the datasheet):

- The masks use 196 distinct bit positions spanning bytes **1..13 and 16..28**.
- Bytes **14 and 15 carry no channel data**.
- Bit positions 96..101, 104..127 are unused by channels.

### Contact quality

One 14-bit value per report, unpacked with `kQualityMask` (bit positions
99..112, i.e. bytes 13..15), describing **one electrode**, selected by byte 0.
The decoder therefore maintains a **sticky** per-channel array: each entry is
the last known value for that electrode, not a per-sample measurement.

The selector mapping (`quality_channel()`) is reproduced from emokit verbatim
and is genuinely odd:

- The block 64..80 mirrors 0..16.
- F8 is reachable from selectors 10 *and* 14, AF4 from 11 and 15, FC6 from 12
  and 16 (and so from 80).

That is the behaviour known to work against hardware. It is not to be
"corrected" without hardware evidence.

Derived refresh rates: since each mapped selector value occurs twice per
counter cycle (once directly, once via the +64 block), most electrodes are
refreshed about **2 Hz**, and F8, AF4 and FC6 about **4 Hz**.

In aggregate: selectors 0..16 and 64..80 are 34 of the 129 counter states, so
**about 34 reports a second carry a fresh reading** and the other ~94 carry
none. Confirmed on the wire — 476 refreshes against 1783 samples, 26.7%,
against a predicted 34/129 = 26.4%.

`Frame::quality_updated` names the electrode a report refreshed, or is empty
when byte 0 held no selector (every battery frame included). It exists
because `Frame::quality` is sticky and so cannot be tested for freshness, and
because comparing the array against the previous frame is *not* an
equivalent test: a refresh that returns the same value is still a fresh
reading. The OSC `/quality/all` message is gated on it.

Units are undocumented. emokit's header treats **> 4000 as a good contact**,
which is what the CLI uses as its bar full-scale. Treat it as a display
convention, not a calibrated threshold.

#### An unresolved overlap

The quality mask (bits 99..112) overlaps the O1 channel mask at bits **102 and
103** — which are O1's two *least significant* bits. Either the table is
slightly wrong or the hardware really does share those positions.

It is essentially undetectable empirically: an error in O1's two LSBs is at
most ±3 counts, about 1.5 µV, well inside the noise floor. Both emokit and
this port do it the same way. Flagged here so nobody re-discovers it and
assumes it is a transcription error in *this* code; do not change it without
hardware evidence.

### Gyro

```cpp
gyro_x = packet[29] - 102;
gyro_y = packet[30] - 104;
```

The offsets are undocumented. emokit's C code uses 102/104; emokit's own
Python port uses 106/105. Neither is justified anywhere. Since the gyro
reports *relative* head motion, a constant offset error only shifts the zero
point — it does not distort motion. The C values are used here and are
confirmed to behave sensibly on hardware.

### Battery

`battery_percent()` maps the raw byte to a percentage through a coarse,
non-linear table reproduced verbatim from emokit:

| Raw | ≥248 | 247 | 246 | 245 | 244 | 243 | 242 | 241 | 240 | 239 | 238 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| **%** | 100 | 99 | 97 | 93 | 89 | 85 | 82 | 77 | 72 | 66 | 62 |

| Raw | 237 | 236 | 235 | 234 | 233 | 232 | 231 | 230 | 229 | 228..226 | else |
|---|---|---|---|---|---|---|---|---|---|---|---|
| **%** | 55 | 46 | 32 | 20 | 12 | 6 | 4 | 3 | 2 | 1 | 0 |

Note the resolution collapses badly below about 60%: the whole 46%..0% range
is 13 raw values. Do not read fine-grained battery trends into this.

## Scaling

`kMicrovoltsPerCount = 0.51` — Emotiv's documented figure for the consumer
EPOC's 14-bit sigma-delta ADC. It is nominal and **not individually calibrated
per headset**, so microvolt output is indicative, not metrological.

The analogue front end band-limits to roughly **0.16–43 Hz**, which is why the
gamma band stops at 45 Hz rather than the 64 Hz Nyquist limit — above that
there is only filter roll-off and mains hum. See [dsp](dsp.md).

## Channel order

```
AF3, F7, F3, FC5, T7, P7, O1, O2, P8, T8, FC6, F4, F8, AF4
```

This is the headset's conventional left-to-right ordering and is used
everywhere in the tool: `Channel` enum values, the display, and the
`/raw/all` OSC message. It is **not** the ordering used by the
contact-quality selector table, which is unrelated; the mapping between them
is handled internally.
