# xavier-tools

Tools for the (discontinued) Emotiv EPOC EEG headset.

The first tool, `epoc`, reads all 14 EEG channels in real time from the
headset's USB receiver dongle and displays them live in the terminal,
along with per-electrode contact quality, gyro and battery.

This is a port of [emokit](https://github.com/openyou/emokit) to modern,
cross-platform C++17 with a standard CMake build. See
[Relationship to emokit](#relationship-to-emokit) for what changed and why.

## Status

| | |
|---|---|
| Windows (MSVC 2022, x64) | Builds, tests pass, dongle enumerates and opens |
| Linux | Build hooks in place, **not yet tested** |
| macOS | Build hooks in place, **not yet tested** |

**The live decode path has not yet been verified against a transmitting
headset.** Everything up to and including opening the dongle and deriving its
decryption key is confirmed working on real hardware; the EEG decode itself is
covered by unit tests against known values, but no powered-on headset has been
available to confirm end-to-end sample output. Treat the first real run as a
verification step.

## Requirements

- **CMake** 3.16 or newer
- A **C++17** compiler (MSVC 2019+, GCC 8+, Clang 7+)
- **Git** — used by CMake to fetch hidapi on first configure
- Network access on first configure, unless you have hidapi installed already

There is no libmcrypt dependency; see [Relationship to emokit](#relationship-to-emokit).

### Platform notes

**Windows** — nothing to install. The dongle works with the built-in HID
driver; no Zadig, no WinUSB, no driver swapping.

**Linux** — install the libudev headers, which hidapi's hidraw backend needs:

```sh
sudo apt install build-essential cmake git libudev-dev   # Debian/Ubuntu
sudo dnf install gcc-c++ cmake git systemd-devel         # Fedora
```

Then install the udev rule, or `epoc` will list the dongle but fail to open it:

```sh
sudo cp packaging/99-emotiv-epoc.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
```

Unplug and replug the dongle afterwards.

**macOS** — Xcode command line tools plus CMake. hidapi's IOKit backend needs
no extra packages.

## Build

```sh
cmake -S . -B build
cmake --build build --config Release
```

On first configure, CMake downloads and statically links hidapi 0.14.0, so the
resulting binary is self-contained. If hidapi is already installed (distro
package, Homebrew, vcpkg) it is used instead.

The binary lands at:

- `build/bin/Release/epoc.exe` — Windows (multi-config generator)
- `build/bin/epoc` — Linux and macOS

### Build options

| Option | Default | Effect |
|---|---|---|
| `XAVIER_BUILD_TESTS` | `ON` | Build the hardware-free test suite |
| `XAVIER_WERROR` | `OFF` | Treat warnings as errors |
| `XAVIER_USE_SYSTEM_HIDAPI` | `ON` | Use an installed hidapi when one is found |
| `XAVIER_HIDAPI_VERSION` | `0.14.0` | hidapi tag to fetch when not using a system one |

For a fully offline build, install hidapi yourself and configure with
`-DXAVIER_USE_SYSTEM_HIDAPI=ON`.

`XAVIER_WERROR` is off by default on purpose: the GCC/Clang warning set
includes `-Wconversion`, which has not yet been exercised on those compilers.
Turn it on once the Linux and macOS builds are clean.

## Test

```sh
ctest --test-dir build -C Release --output-on-failure
```

No hardware required. The suite covers AES-128 against the FIPS-197 reference
vector, key derivation for both headset variants, bit unpacking, the battery
table, and packet decode including the sticky battery and contact-quality
fields. This matters more than usual here: the original code was only ever
validated against live hardware, and the EPOC is discontinued, so the decode
path needs to stay checkable without a headset on the bench.

## Run

Plug in the dongle, switch on the headset, then:

```sh
./build/bin/Release/epoc        # Windows
./build/bin/epoc                # Linux/macOS
```

Ctrl+C to quit. Output looks like:

```
epoc 0.1.0 -- live EEG
serial SN201302043668GM   variant consumer (detected)

 chan      counts           uV     p2p/s uV   contact quality
 ----      ------           --     --------   ---------------
 AF3         8412       4290.1         12.2   ##########----  4381
 F7          8390       4278.9          9.7   #########-----  3902
 ...

 battery  87%    gyro x   +3  y   -1    seq  42
 samples  12345  drops 0         rate  127.9 Hz
```

Both raw ADC counts and microvolts are shown. `p2p/s uV` is peak-to-peak over
the last second, which is the quickest way to tell a live electrode from a
flat one. `drops` counts packets the dongle numbered but that never arrived.

### Reading the numbers

- **counts** are raw 14-bit ADC values, exactly as emokit reports them. This
  is the lossless figure; prefer it if you are going to do your own analysis.
- **uV** applies Emotiv's nominal 0.51 uV/count. It is *not* individually
  calibrated per headset, so treat it as indicative, not metrological.
- **contact quality** is in undocumented raw units. emokit's header notes that
  above ~4000 is a good contact, which is the convention the bar uses. The
  dongle reports quality for one electrode per packet, so these values refresh
  at 1-16 Hz, not per sample.

### Options

```
-l, --list            list connected Emotiv dongles and exit
-i, --index N         open the Nth dongle (default: probe automatically)
    --path PATH       open a specific HID path (see --list)
    --serial SN       override the serial used for key derivation
    --consumer        force the consumer key layout
    --research        force the research key layout
    --show-key        print the derived AES key and exit (debugging)
    --refresh MS      display refresh interval, ms (default: 50)
    --settle MS       per-interface probe timeout, ms (default: 2000)
-h, --help            show this help and exit
-V, --version         show version and exit
```

Redirecting output to a file or pipe switches to an append-only one-line-per-
refresh format instead of the refreshing display, so a redirect stays readable.

## Troubleshooting

**"no Emotiv dongle found"** — the receiver is not plugged in, or not
recognised. Check with `epoc --list`. The dongle is USB `21a1:0001`.

**"found the dongle but no interface is streaming EEG data"** — the dongle is
fine; the headset is not transmitting. Switch it on, check it is charged, and
confirm it is paired with *this* dongle. Try `--settle 5000` to wait longer.

The dongle presents **two** HID interfaces and only one carries EEG reports.
The other opens without error and then stays silent forever, which is a
genuinely confusing failure mode. `epoc` probes both automatically, preferring
interface 1, and tells you which one it settled on. `--list` shows both.

**"could not open HID path"** — on Linux this is almost always the missing
udev rule (see above). On Windows, close any other software holding the dongle,
including Emotiv's own SDK or control panel.

**Channels read as plausible-looking noise** — the decryption key is probably
wrong. The dongle encrypts its output with AES-128 against a key derived from
its own serial number, and the layout differs between consumer and research
headsets. `epoc` detects the variant from a HID feature report, but that
detection can fail. Try `--research` (or `--consumer`) explicitly, and use
`--show-key` to inspect what is being derived.

Note also that emokit's protocol document and emokit's own source code
disagree on the consumer key layout. `epoc` implements the source-code version,
since that is the one known to have worked against real hardware. If neither
variant works, this discrepancy is the first thing to investigate — see the
note in `epoc/src/protocol.cpp`.

## Layout

```
epoc/
  include/epoc/epoc.hpp   public API: Device, Frame, Channel, protocol primitives
  src/aes128.{hpp,cpp}    AES-128 ECB decryption (replaces libmcrypt)
  src/protocol.cpp        bit unpacking, key derivation, battery/quality tables
  src/device.cpp          hidapi transport, RAII device handle
  cli/                    the epoc command line tool
  test/                   hardware-free tests
cmake/GetHIDAPI.cmake     finds or fetches hidapi
packaging/                Linux udev rule
```

The decode and transport logic lives in a static library (`epoc`) separate
from the CLI, so further tools in this repo can reuse it.

## Relationship to emokit

This is a port of [emokit](https://github.com/openyou/emokit) (Copyright (c)
2010 Daeken and Skadge; Copyright (c) 2011-2012 OpenYou Organization). The
wire protocol handling is unchanged, and the bit masks and lookup tables are
reproduced verbatim.

What changed:

- **libmcrypt is gone.** emokit used it for one thing: ECB-decrypting two
  16-byte blocks per report. libmcrypt is unmaintained and effectively
  unobtainable on Windows, so it is replaced by a ~160-line AES-128
  decrypt-only implementation, verified against the FIPS-197 reference vector.
  This removes the port's hardest dependency on all three platforms.
- **C++17 with RAII.** `Device` closes itself; no manual create/delete pairs,
  and errors are exceptions carrying actionable messages rather than integer
  codes.
- **CMake modernised** from the 2.6-era script: targets and usage requirements
  instead of directory-wide `INCLUDE_DIRECTORIES` and a global `LIBS` variable.
  hidapi is fetched automatically rather than hunted for by a hand-written
  `FindHIDAPI.cmake`.
- **Automatic interface probing.** emokit's example hardcoded "try device
  index 1, then 0" with no explanation. That preference is now encoded and
  documented, and the tool reports which interface it chose.
- **Two latent bugs fixed:**
  - Key derivation indexed the serial at a hardcoded offset of 16, silently
    producing a wrong key for any serial that was not exactly 16 characters.
    It now indexes from the actual end of the string.
  - The serial was read from a `wchar_t` buffer by casting each element to a
    byte, which only works where `wchar_t` is 16-bit — i.e. on Windows, but
    not on Linux or macOS. It is now narrowed explicitly.
- **Uninitialised frame on error.** `emokit_get_next_frame` returned a
  stack-garbage frame with only `counter` set if decryption failed. Decoding
  now writes into a caller-owned, fully-initialised `Frame`.

Two discrepancies in the original are preserved rather than silently "fixed",
and are flagged in comments where they occur: the consumer key layout
disagreement between emokit's docs and its code, and the gyro zero offsets
(102/104 in the C source, 106/105 in emokit's own Python port).

## Licence

**This project's own licence has not been chosen yet** -- see `LICENSE`.

Portions derived from emokit retain their original copyright and licence
terms, which are reproduced in `LICENSE`. Upstream is inconsistent about
which licence applies: emokit's per-file headers carry an ISC-style
permission notice, while its `LICENSE` file is a public-domain dedication
with a 3-clause-BSD fallback. Both texts are included.

hidapi is fetched at build time, is not vendored into this repository, and
carries its own licence (GPLv3 / BSD / original HIDAPI licence, at your
option).
