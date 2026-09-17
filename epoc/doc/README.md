# epoc — internals

Documentation for the EPOC tool's internals. User-facing instructions (build,
run, options, troubleshooting) are in the [top-level README](../../README.md).

| Document | What it covers |
|---|---|
| [architecture.md](architecture.md) | How the tool is layered: what each translation unit owns, and the one deliberate exception (`OscStreamer` in `main.cpp`). |
| [protocol.md](protocol.md) | The wire protocol: USB/HID, the two-interface behaviour, AES key derivation, report layout, bit unpacking, quality, gyro, battery. **Start here** — it is the knowledge that is hardest to re-derive. |
| [dsp.md](dsp.md) | Band powers: pipeline, normalisation maths, window trade-offs, how the numbers are verified. |
| [osc.md](osc.md) | OSC streaming: message set, encoding rules, timestamp formats and receiver interoperability, networking. |
| [testing.md](testing.md) | What the 162 assertions cover, what they do not, and conventions for adding more. |
| [decisions.md](decisions.md) | Why things are the way they are, including the alternatives that were rejected and one decision that was reversed and reinstated. |

Repo-wide documentation lives in [`../../doc/`](../../doc/architecture.md):
architecture and layering, development workflow, and current project status.
