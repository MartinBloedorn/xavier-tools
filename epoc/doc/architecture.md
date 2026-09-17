# Architecture

## Layering

The dependency direction is strictly downward. Nothing below reaches up.

```
             cli/main.cpp                 argument parsing, display, OSC driving
                  |
    +-------------+-------------+
    |             |             |
  dsp.hpp      osc.hpp      epoc.hpp      public API
    |             |             |
  dsp.cpp      osc.cpp     device.cpp     implementation
                                |
                          protocol.cpp    pure functions, no I/O
                          aes128.cpp      pure functions, no I/O
                                |
                             hidapi        PRIVATE — never in a public header
```

Three properties are worth preserving:

**The protocol layer is pure.** `derive_key`, `unpack_level`,
`battery_percent` and `decode_packet` take bytes and return values. They touch
no hardware, allocate nothing in the hot path, and are `noexcept` where they
can be. This is what makes the decode path testable without a headset — which
matters more than usual here, because the EPOC is discontinued and the
reference implementation was only ever validated against live hardware.

**hidapi is an implementation detail.** It appears in no public header and is
linked `PRIVATE`. A consumer of `libepoc` never sees a `hid_device*`. If the
transport is ever swapped (libusb, a replay-from-file harness), only
`device.cpp` changes.

**The CLI owns no protocol knowledge.** `main.cpp` parses arguments, drives the
read loop, and renders. Every fact about the wire format lives in the library.
The one deliberate exception is `OscStreamer`, which lives in `main.cpp`
because it is policy (which messages to emit, when) rather than encoding
(`osc.cpp` owns the bytes). See [OSC design](osc.md).

## Build system

Standard CMake, minimum 3.16, C++17 with extensions off.

| Target | Kind | Notes |
|---|---|---|
| `xavier_warnings` | INTERFACE | Carries warning flags. Not applied globally, so fetched third-party code is not held to our warning level. |
| `hidapi::hidapi` | IMPORTED/ALIAS | Provided by `cmake/GetHIDAPI.cmake`, whatever its origin. |
| `epoc` | STATIC | The library. Output name `epoc`. |
| `epoc_cli` | EXECUTABLE | `OUTPUT_NAME epoc` — the CMake target name differs from the on-disk name to avoid colliding with the library target. |
| `epoc_tests`, `epoc_dsp_tests`, `epoc_osc_tests` | EXECUTABLE | Registered with CTest as `epoc_protocol`, `epoc_dsp`, `epoc_osc`. |

Binaries land in `<build>/bin` and libraries in `<build>/lib`, regardless of
generator. Multi-config generators (Visual Studio, Xcode) append the config
name — so on Windows the CLI is at `<build>/bin/Release/epoc.exe`.

### Options

| Option | Default | Purpose |
|---|---|---|
| `XAVIER_BUILD_TESTS` | `ON` | Build the three test executables |
| `XAVIER_WERROR` | `OFF` | Warnings as errors — see the caveat below |
| `XAVIER_USE_SYSTEM_HIDAPI` | `ON` | Prefer an installed hidapi when one is found |
| `XAVIER_HIDAPI_VERSION` | `0.14.0` | Tag to fetch when building hidapi ourselves |

`XAVIER_WERROR` is deliberately off. The GCC/Clang warning set includes
`-Wconversion` and `-Wsign-conversion`, which have never been exercised — the
code has only ever been compiled with MSVC. Turning `XAVIER_WERROR=ON` on the
first Linux build is a good way to find the gaps, but expect to fix some
narrowing conversions before it passes.

The version string has one source of truth: the `project(... VERSION)` call,
passed to the CLI as `XAVIER_EPOC_VERSION` via `target_compile_definitions`.
`main.cpp` carries an `#ifndef` fallback of `"unknown"` so it still compiles if
built outside CMake.

### Dependency policy

One external dependency: hidapi. `cmake/GetHIDAPI.cmake` resolves it in three
steps, stopping at the first that works, and reports which in the configure
summary via `XAVIER_HIDAPI_ORIGIN`:

1. `find_package(hidapi CONFIG)` — a proper installed package.
2. `pkg-config`, trying `hidapi-hidraw`, then `hidapi`, then `hidapi-libusb`.
   This is the usual Linux path.
3. `FetchContent`, pinned to the tag in `XAVIER_HIDAPI_VERSION`, built static.

A subtlety worth knowing if you touch that file: a target produced by
`pkg_check_modules(... IMPORTED_TARGET)` is *not* `GLOBAL`, so it cannot be
`ALIAS`ed directly. It is wrapped in an INTERFACE library first:

```cmake
add_library(xavier_hidapi_pc INTERFACE)
target_link_libraries(xavier_hidapi_pc INTERFACE PkgConfig::_HIDAPI)
add_library(hidapi::hidapi ALIAS xavier_hidapi_pc)
```

Whatever the origin, everything downstream links the single name
`hidapi::hidapi`.

## Cross-platform strategy

The port targets Windows, Linux and macOS. Only Windows has actually been
built and run — see [status](../../doc/status.md) — but every platform-specific decision
is already made and isolated. There are exactly four places where the platform
matters:

| Concern | Where | How |
|---|---|---|
| HID transport | `device.cpp` | Delegated entirely to hidapi (Windows HID API / Linux hidraw / macOS IOKit) |
| UDP sockets | `osc.cpp` | `#ifdef _WIN32` — Winsock vs BSD sockets, refcounted `WSAStartup` |
| Terminal control | `cli/terminal.cpp` | `ENABLE_VIRTUAL_TERMINAL_PROCESSING` + `GetConsoleScreenBufferInfo` vs `isatty` + `TIOCGWINSZ` |
| Link libraries | `epoc/CMakeLists.txt` | Apple frameworks (IOKit, CoreFoundation, AppKit), UNIX `Threads::Threads`, Windows `ws2_32` |

Two portability details that are easy to get wrong and are already handled:

- **`wchar_t` width.** hidapi returns `wchar_t*` strings. `wchar_t` is 16-bit
  on Windows and 32-bit elsewhere, so emokit's byte-cast was only ever correct
  on Windows. `device.cpp::narrow()` walks code points and narrows ASCII
  explicitly.
- **Report IDs.** The dongle's reports carry no report id, and hidapi already
  normalises away the leading zero byte Windows prepends. A 32-byte read is
  correct everywhere; do not add a platform offset.

Linux additionally needs device permissions — `packaging/99-emotiv-epoc.rules`
grants them via `TAG+="uaccess"` with a `plugdev`/`0660` fallback, covering
both the `hidraw` and `usb` subsystems.

## Conventions

- **C++17**, no compiler extensions. No exceptions to that; the code avoids
  anything that would need `/permissive`.
- **Errors**: `epoc::Error` (derives `std::runtime_error`) for anything a user
  can act on, with a message that says what to do next — the "could not open
  HID path" message names the udev rule, for instance. A read *timeout* is not
  an error: `Device::read_frame` returns `bool`, because a silent headset is a
  routine, recoverable state.
- **Ownership**: `Device` is move-only and uses pImpl, so the header never
  leaks hidapi types. `HidGuard`/`WinsockLibrary` are refcounted RAII wrappers
  around global init/teardown that the underlying libraries do not refcount
  themselves.
- **`std::optional`** for "might not be there" returns (`open_streaming`,
  option values), not sentinel values.
- **Comments explain the non-obvious.** Verbatim tables from emokit say so.
  Deviations from emokit say so *and* say why, at the point they occur. Values
  that look wrong but are right (the duplicated quality selectors, the removed
  key layout) carry the evidence. Do not "tidy" these away.
- **Warnings**: MSVC `/W4 /permissive- /utf-8`; GCC/Clang `-Wall -Wextra
  -Wpedantic -Wshadow -Wconversion -Wsign-conversion`.

## Adding a second tool

The repository is laid out to hold several. To add one:

1. Create `<tool>/` with the same shape: `include/<tool>/`, `src/`, `cli/`,
   `test/`, `doc/`.
2. `add_subdirectory(<tool>)` in the top-level `CMakeLists.txt`.
3. Link `xavier_warnings` on every target you own; do not set warning flags
   globally.
4. If it needs the EPOC, link the `epoc` target — that is what the public
   headers and the install rules are for. Do not reach into `epoc/src/`.
   (The protocol tests do, deliberately, to reach `aes128.hpp`; that is the
   only sanctioned exception and it is explicit in the CMake file.)
