# Architecture

How `xavier-tools` is put together, and why. This is the orientation document:
read it first when picking up the project cold.

Tool-specific internals live under the tool's own `doc/` directory —
[`epoc/doc/`](../epoc/doc/README.md) for the acquisition tool and
[`view/doc/`](../view/doc/architecture.md) for the viewer.

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
  view/                   xavier-viewer — the web view for the OSC stream
    xavier-viewer         entry point (Python 3.7+, no dependencies)
    viewer/               backend: OSC decode, stream state, HTTP and SSE
      web/                frontend: vanilla JS, canvas plotting, no build step
    tools/                synthetic EPOC generator, headless-browser driver
    doc/                  viewer-specific documentation
```

The two tools share no code and are built and run independently — the viewer
consumes `epoc`'s OSC output over UDP, which is the whole interface between
them. That is deliberate: it also means any other OSC source, or a recording
played back, drives the viewer just as well.
