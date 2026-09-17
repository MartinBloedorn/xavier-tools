# Architecture

How `xavier-tools` is put together, and why. This is the orientation document:
read it first when picking up the project cold.

Tool-specific internals live under the tool's own `doc/` directory —
currently only [`epoc/doc/`](../epoc/doc/README.md).

## Repository layout

```
xavier-tools/
  CMakeLists.txt          top-level: standards, warnings, options, summary
  cmake/GetHIDAPI.cmake   dependency resolution for hidapi
  doc/                    repo-wide docs (this directory)
  packaging/              udev rule for Linux
  epoc/                   the EPOC tool
    CMakeLists.txt        library + CLI + three test targets
    include/epoc/         public headers — the library's whole API surface
      epoc.hpp              device, frames, protocol primitives
      dsp.hpp               FFT and band powers
      osc.hpp               OSC encoding and UDP transmission
    src/                  implementation, plus private headers
      aes128.{hpp,cpp}      AES-128 ECB decrypt, no external crypto library
      protocol.cpp          bit unpacking, key derivation, lookup tables
      device.cpp            hidapi transport
      dsp.cpp               FFT, Hann window, band integration
      osc.cpp               OSC serialisation, UDP, option parsing helpers
    cli/                  the executable — argument parsing and display only
      main.cpp
      terminal.{hpp,cpp}    ANSI/VT handling, tty detection, signal handling
    test/                 hardware-free tests
      check.hpp             shared assertion helpers
      test_epoc.cpp         protocol layer
      test_dsp.cpp          spectral analysis
      test_osc.cpp          OSC encoding and option parsing
    doc/                  EPOC-specific documentation
```
