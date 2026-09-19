# xavier-viewer

A web view for the OSC stream produced by the [`epoc`](../README.md)
acquisition tool: fourteen live EEG traces, band powers, contact quality,
gyro and battery, in a browser.

A Python backend receives the OSC, a browser draws it. Both halves are
dependency-free — stock Python 3.7+, no pip install, no CDN, no build step.

```
epoc --osc 9000 --osc-messages tbugyq --osc-timestamp timetag
./xavier-viewer
```

The viewer opens a browser at <http://127.0.0.1:8420/>.

On Windows, `xavier-viewer.cmd` is the same thing from `cmd.exe` or
PowerShell; `python xavier-viewer` works anywhere.

## What it expects

The message set above is not a suggestion — these are the five addresses the
viewer reads:

| Address | Used for |
|---|---|
| `<prefix>/raw/all` | the scrolling traces |
| `<prefix>/quality/all` | the contact-quality bars |
| `<prefix>/fft/<channel>` | the band-power bars |
| `<prefix>/gyro/x`, `<prefix>/gyro/y` | the gyro widget |
| `<prefix>/battery` | the battery indicator |

`u` rather than `r`: `/raw/all` carries the same samples as the fourteen
per-channel messages in a fourteenth of the packets. `q` adds
`/quality/all`, and only does so when paired with `u`. A stream sent with
`r` instead of `u` still registers as traffic — the link shows as live — but
nothing is plotted.

Any of the four `--osc-timestamp` encodings is accepted. `timetag` is the
canonical OSC type and what the viewer is built around; `int64` and `double`
work identically. `int32` is milliseconds since the DAT started, which has
no relationship to the viewer's clock, so arrival time is used instead —
fine for a live display, but the timestamps are then the viewer's, not the
acquisition's.

If nothing appears, check the prefix first. The viewer watches for OSC that
arrives but does not match, and says so in the top bar:
*"OSC arriving on /epoc — prefix mismatch?"*

## Without a headset

`tools/fake_dat.py` sends synthetic EPOC traffic over real UDP — same
addresses, same argument types, same rates — so the whole receive path is
exercised:

```
python tools/fake_dat.py --osc 9000
```

The signal is synthetic EEG: a DC offset around 8400 counts, a per-channel
alpha rhythm strongest at the occipital electrodes, pink-ish noise, and the
occasional frontal blink artefact. Band powers are computed with a real FFT
using the same normalisation as `epoc/src/dsp.cpp`, so a 14 µV alpha rhythm
reads as roughly 98 µV² — its closed-form A²/2 — on the bars.

## The interface

**Top bar** — OSC port and prefix (editable; `apply` re-points the socket
live and saves the change), link state and measured sample rate, battery,
and a play/pause button. Space bar toggles pause. Pausing freezes the plots;
the link indicator and battery keep updating, because a frozen connection
status would be a lie.

**Widgets** open side by side in columns, each filling the height. Drag the
divider between two to resize; double-click it to restore an even split. No
widget can be squeezed below a sixth of the window width.

Each widget's parameters live in a bar along its bottom, two rows tall, and
every bar is the same height. The chevron at the right end collapses all of
them at once — same height across widgets is the point, so the collapse is
shared. On the data viewer the top row is channel selection and the bottom
row everything else; on the gyro viewer the top row is what to show, which way
round, and over what span; the bottom row the integrator's own settings.

### Data viewer

One row per channel, stacked to fill the height.

The **scrolling trace** reads against the left axis (value) and the bottom
axis (time). The **band-power bars** read against the right axis (power) and
the top axis (frequency), each bar spanning its own band's frequency range.
Both share a row because they describe the same electrode. The trace is drawn
at full saturation over the bars, which are a wash of amber behind it.

| Control | |
|---|---|
| channel chips | one per electrode; **alt-click solos** a channel |
| `all` / `none` | every channel on or off |
| `raw` / `fft` / `quality` | which layers are drawn |
| `µV` / `raw` | microvolts (0.51 µV/count, nominal) or ADC counts |
| scale | `auto` per channel, or a fixed ± range |
| `ac` | subtract the window mean — see below |
| window | 2 to 30 seconds of scrollback |
| `log` / `lin` | band-power axis; log spans three decades below the peak |
| `shared` | one band-power range across all channels, so they compare |

**`ac` is on by default and you almost always want it.** EPOC counts sit on
a DC offset of about 8400 — roughly 4.3 mV, three orders of magnitude above
the rhythms. Without the mean subtracted, every trace is a flat line. This
is the same reasoning as the mean removal in the band-power pipeline; see
[epoc/doc/dsp.md](../epoc/doc/dsp.md#why-the-mean-is-removed).

Contact quality shows as a coloured bar in the left gutter and a number
beside each channel name. Full scale is emokit's "> 4000 is a good contact",
which is a display convention rather than a calibrated threshold.

### Gyro viewer

A spirit-level bubble on top, a scrolling graph below.

The bubble carries two dots: the raw **rate** (x, y) and the integrated
**position** (yaw, pitch), the latter with a short trail. Each is scaled
independently — they have unrelated magnitudes — and its full scale is
printed beside the rings.

Position is estimated by **high-passed integration**: a plain integrator
walks off to infinity on the smallest bias, so the integral is leaked back
toward zero,

```
angle[n] = a * angle[n-1] + rate * dt,    a = exp(-2*pi*fc*dt)
```

which is a first-order high pass with time constant `1/(2*pi*fc)`. The
`high-pass` slider sets `fc` and shows both numbers; the time constant is
the one worth reading — it is how long the estimate takes to forget. A
constant bias then parks the output at a constant offset instead of ramping
away. `zero` takes the last second as the resting rate and re-derives the
whole visible history from it.

`flip x` and `flip y` invert an axis, for when the headset's idea of which
way is which does not match yours. One toggle covers the rate and the angle
derived from it together — yaw is the integral of x — and the visible
history flips with it, so the graph does not gain a step where you clicked.
Nothing in the protocol fixes the sign, so this is a preference rather than
a correction; it is saved with the rest of the settings.

Units are **a.u.** The EPOC's gyro scale is undocumented and the zero offsets
in the decoder are inherited from emokit without justification, so calling
the output degrees would be inventing a calibration that does not exist. See
[epoc/doc/protocol.md](../epoc/doc/protocol.md#gyro).

## Configuration

`viewer.conf.json`, written next to the entry point, holds the whole state:
the OSC endpoint, the HTTP port, which widgets are open and how wide, and
every widget's parameters. It is created on first run and saved as you go,
so the viewer comes back the way you left it.

Command-line options override it **and are written back**, so
`--osc-port 9100` once is the same as editing the file.

```
  --osc-port N      UDP port to receive OSC on (default: 9000)
  --osc-bind ADDR   address to bind the OSC socket to (default: 0.0.0.0)
  --osc-prefix P    prefix the DAT is using (default: /epoc)
  --port N          HTTP port for the web view (default: 8420)
  --host ADDR       address to serve the web view on (default: 127.0.0.1)
  --config FILE     configuration file
  --no-browser      do not open a browser on startup
  --reset           ignore the saved configuration, start from defaults
  --verbose         log HTTP requests
```

The OSC socket binds `0.0.0.0` and the web view binds `127.0.0.1`: the
acquisition tool may well be running on the machine the dongle is plugged
into rather than on this one, but the web view has no authentication and
should not be offered to the network without a deliberate `--host`.

## Analysis output

The backend can emit OSC of its own, for analysis tools living in the
viewer. Nothing uses it yet — it is the seam, built and tested from the
start so the first tool to need it finds a working path:

```
POST /api/output  {"enabled": true, "host": "127.0.0.1", "port": 9100}
POST /api/emit    {"address": "focus", "args": [0.42]}   -> /xavier/focus
```

## Development

| | |
|---|---|
| [doc/architecture.md](doc/architecture.md) | How the pieces fit, and the decisions worth knowing about |
| `tools/fake_dat.py` | Synthetic EPOC over real UDP |
| `tools/cdp.py` | Drives headless Chrome over the DevTools protocol — screenshots, console errors, evaluating JS against the running page |

There is no build step. Edit and reload; every asset is served `no-store`.

`tools/cdp.py` exists because the viewer holds an SSE connection open for as
long as the page is up, so the page never reaches "load finished" and
`chrome --screenshot` waits forever:

```
python tools/cdp.py http://127.0.0.1:8420/ --wait 5 --shot out.png
python tools/cdp.py http://127.0.0.1:8420/ --eval "xavier.panels.get('data').instance.ring.size"
```

`window.xavier` is a deliberate handle on the running app for exactly this.
