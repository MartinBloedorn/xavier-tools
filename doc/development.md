# Development workflow

Practical notes for working on `xavier-tools`: how to build, how to verify
changes, and the environment traps that have actually cost time.

See [architecture](architecture.md) for how the code is organised and
[status](status.md) for what is and is not verified.

## Build and test

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

`--config Release` matters only for multi-config generators (Visual Studio,
Xcode); single-config generators take the build type at configure time and
default to Release here. On Windows the CLI ends up at
`build/bin/Release/epoc.exe`; elsewhere at `build/bin/epoc`.

The three CTest suites — `epoc_protocol`, `epoc_dsp`, `epoc_osc` — need no
hardware and run in about 3 seconds. Run them on every change; they are the
only regression safety net, since the hardware is discontinued and not
available in CI.

## Windows specifics

**Kill a running `epoc.exe` before rebuilding.** The single most common build
failure here is:

```
LNK1104: cannot open file '...\bin\Release\epoc.exe'
```

That is not a code error. It is a still-running instance holding the binary —
usually one left behind by a background OSC test. Prefix builds with:

```powershell
Get-Process epoc -ErrorAction SilentlyContinue | Stop-Process -Force
```

**hidapi is built `/MD`.** If you write a throwaway probe and compile it by
hand with `cl`, the default is `/MT`, and you get a wall of `LNK2019
unresolved external __imp__wcsdup`-style errors. Add `/MD`. This does not
affect the CMake build, which sets it consistently.

**`Get-PnpDevice` filtering is unreliable for this dongle.** Trying to confirm
the dongle is present with `Get-PnpDevice -InstanceId -match 'VID_21A1'`
returned nothing while the device was plugged in and working. Use the tool's
own enumeration instead:

```powershell
.\build\bin\Release\epoc.exe --list
```

That is the authoritative answer, and it prints both HID interfaces with their
paths and interface numbers.

## Working without a headset

Most of the tool can be exercised with no hardware at all:

| Task | How |
|---|---|
| Whole decode path | `ctest` — AES, key derivation, bit unpacking, battery, packet decode |
| Key derivation for a given serial | `epoc --show-key --serial SN0123456789ABCD` (pure; needs no device) |
| OSC encoding, flags, endpoints | `epoc_osc_tests` |
| Band power maths | `epoc_dsp_tests` — synthetic sinusoids with known amplitude |

With a dongle but no live headset, two more paths open up:

| Task | How |
|---|---|
| Enumeration and both interfaces | `epoc --list` |
| The wait-for-headset loop | `epoc --index 0` — interface 0 opens cleanly and never streams, so it drops into the wait loop deterministically |
| The immediate-failure path | `epoc --index 0 --no-wait` |

`--index 0` is the cheap deterministic way to test waiting behaviour. Note the
transition *out* of the wait loop, when a sleeping headset wakes, has never
been observed directly — it needs a remote power cycle. It reuses the same
`probe_once()` that the initial probe uses, which is verified.

## Verifying OSC on the wire

Do not trust a receiver application's rendering; decode the bytes. A
third-party receiver already produced one false bug report by truncating an
int64 to 32 bits (see [OSC design](../epoc/doc/osc.md#timestamp-encoding)).

A minimal receiver that counts messages by address and decodes the first
`/battery` packet:

```python
import socket, struct, sys, time, datetime
PORT = int(sys.argv[1]); SECONDS = float(sys.argv[2])
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(('127.0.0.1', PORT)); s.settimeout(0.5)
counts, first, deadline = {}, None, time.time() + SECONDS
while time.time() < deadline:
    try:
        d, _ = s.recvfrom(65535)
    except socket.timeout:
        continue
    end = d.index(b'\0'); addr = d[:end].decode()
    counts[addr] = counts.get(addr, 0) + 1
    if addr.endswith('/battery') and first is None:
        i = (end // 4 + 1) * 4
        end2 = d.index(b'\0', i); tags = d[i:end2].decode()
        j = (end2 // 4 + 1) * 4
        first = (tags, struct.unpack_from('>q', d, j)[0],
                 struct.unpack_from('>f', d, j + 8)[0])
for a in sorted(counts):
    print('%-20s %5d' % (a, counts[a]))
print('battery:', first)
```

Two things that recipe encodes, both of which are easy to get wrong when
hand-rolling an OSC parser:

- OSC strings are NUL-terminated and padded to a **multiple of four**, so the
  next field starts at `(len // 4 + 1) * 4` — the `+ 1` is because there is
  always at least one terminator, even when the length is already a multiple
  of four.
- All numeric arguments are **big-endian**, regardless of host byte order.

Run the receiver and the tool together, then compare against the startup
banner, which reports the resolved numeric address:

```
epoc: streaming OSC to localhost:10131 (127.0.0.1), prefix /epoc, messages [tgy], timestamp int64
```

Bind the receiver to `127.0.0.1` specifically rather than `0.0.0.0` when
testing address resolution — a receiver bound to all interfaces will happily
accept IPv6 traffic and hide a resolution bug.

## Message rates, for sizing a test

At 128 Hz:

| Flags | Messages/second | Note |
|---|---|---|
| `r` | ~1800 | 14 per sample — the heavy one |
| `u` | 128 | Same data, one message |
| `g` | 256 | Two axes |
| `b` | ~280 | 14 per spectrum, once per `--refresh` (default 50 ms) |
| `y` | ~0.2 | On change, plus a resend every 5 s |

A ten-second `tgy` run should produce roughly 1280 gyro-x, 1280 gyro-y and
two battery messages — one about a second in, the resend about five seconds
later. Counts far off that mean something is wrong.

## Editing note

Several source files contain C string literals with embedded escape sequences
and Markdown with backticks. Writing them through a shell heredoc has
repeatedly mangled them — backslash-`n` collapsing to a real newline,
backticks being interpreted as command substitution. Use a file-writing tool
directly, or write a Python edit script to a scratch directory and run it with
`python script.py`. Do not pipe multi-line C++ or Markdown through the shell.

## Conventions when changing the protocol layer

The tables in `protocol.cpp` are reproduced verbatim from emokit and are
validated only by hardware behaviour. Some of them look wrong (the
contact-quality selector table has duplicates; the 64..80 block mirrors
0..16). They are not to be "corrected" without hardware evidence — the comment
at each one explains what is known. If you do change one, say what you tested
it against, and add a test that pins the new behaviour.

Anything that deviates from emokit should carry a comment at the point of
deviation explaining both the change and the reason, in the style of the ones
already there. Several existing deviations are genuine bug fixes to emokit
(serial indexing from the end, `wchar_t` narrowing) and the comments are the
only record of that.
