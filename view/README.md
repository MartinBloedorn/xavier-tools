# xavier-viewer

A web view for the OSC stream produced by the [`epoc`](../README.md)
acquisition tool: fourteen live EEG traces, band powers, contact quality,
gyro and battery, in a browser — and an analysis tool that turns the band
powers into a valence/arousal point and a pair of cognitive indices, and
sends them back out as OSC.

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

**Top bar**, left to right: the **in** endpoint (OSC port and prefix —
editable; `apply` re-points the socket live and saves the change), link
state and measured sample rate with the battery, and the **out** endpoint
(port and prefix for what the analysis tool emits, and the `send` switch
that gates it). Then a play/pause button — space bar toggles it too.

Pausing freezes the plots and stops the analysis output with them; the link
indicator and battery keep updating, because a frozen connection status
would be a lie.

The output host is `127.0.0.1` and is editable in `viewer.conf.json` only:
the field would crowd a bar that already carries two endpoints, and the
common case is a synth on the same machine.

**Widgets** open side by side in columns, each filling the height. Drag the
divider between two to resize; double-click it to restore an even split. No
widget can be squeezed below a sixth of the window width.

Each widget's parameters live in a bar along its bottom, two rows tall, and
every bar is the same height. The chevron at the right end collapses all of
them at once — same height across widgets is the point, so the collapse is
shared. On the data viewer the top row is channel selection and the bottom
row everything else; on the gyro viewer the top row is what to show, which way
round, and over what span; the bottom row the integrator's own settings; on
the analysis tool the top row is the emotion model and the bottom row the
cognitive pair.

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

The estimated position can be sent on as OSC. `osc send` streams

```
<prefix>/gyro/yaw
<prefix>/gyro/pitch
```

as floats, at up to the rate beside the switch, in exactly the units shown
here — after the flips, the gain and the high pass. Deliberately *not*
normalised against the bubble's auto-scale: that scale follows the last few
seconds of movement, so the same head position would leave as a different
number depending on what happened before it, which is indefensible for
something driving a synth. `gain` is the knob for making the magnitude suit
a receiver, and it holds still.

The rates are not sent. A rate is what the headset already broadcasts, and
anything that wants one can subscribe to `/epoc/gyro/x` directly; the
estimate is the thing this widget makes. As with the analysis tool, the top
bar's `out` switch gates the stream, the widget goes on sending while it is
closed, and the corner of the bubble says whether it is running and how fast
it actually is.

### Analysis tool

Two analyses over the band powers, drawn one above the other: a 2D emotion
model, and a pair of cognitive indices. The maths and the channel choices
are from [doc/analysis.md](doc/analysis.md); what follows is what the widget
does with them.

```
valence = ln(alpha F4) - ln(alpha F3)     frontal alpha asymmetry
arousal = ln(beta / alpha)                summed over F3 F4 F7 F8
focus   = ln(beta / (alpha + theta))      summed over F3 F4 F7 F8
relax   = ln(alpha O1 + alpha O2)         absolute occipital alpha
```

Each index is then low-passed, measured against a **baseline**, and mapped
into its display range. The baseline is what makes the numbers mean
anything: absolute band power says as much about the skull and the electrode
contact as about the mind underneath, so only the departure from this head's
own resting level is reported.

**A held state fades.** Concentrate for a minute against a 30 s baseline and
focus rises and then settles back toward the middle, because the rolling
baseline has followed you there. That is what a trailing baseline is, and it
is the right behaviour for an installation that should respond to change.
When a value has to mean the same thing all evening, press `calibrate`: it
averages the next ten seconds and then freezes the baseline there. The
button reads `cancel` while collecting and `release` once held, and the
emotion header says which mode is in force.

The **grid** is valence (right positive) against arousal (up positive), with
Russell's quadrant names in the corners and an eight-second trail behind the
dot. Under it, a **graph** of the same two indices against time — valence
solid, arousal dashed — and then the electrodes the model is built on, each
with its contact quality — a reading taken from an electrode that is not seated is not a
reading, and this is the failure the widget is least likely to show
otherwise.

The **faders** are separate rather than a second grid: focus and relaxation
come from different lobes and move independently, and a pair of coordinates
would imply a relationship the maths does not have. Focus rests at
mid-scale, with a tick to show where that is. Relax rests at zero and rises,
with a trigger mark: it is the eye-close alpha spike, which is a switch
rather than a balance. A second graph below them plots both.

Both graphs span the **baseline window**, which is exactly the stretch of
time the current zero was computed from — so their mid-line is both "no
deviation" and "the average of what is drawn". They are what turns a slow
drift into something visible as a drift rather than as a dot that happens to
be over there now, and either can be switched off with `graph`. In a widget
too short for both, both go: one quietly missing would read as a fault in
whichever index lost the draw, and they are there to be compared.

| Control | |
|---|---|
| `on` | compute and draw that analysis |
| `osc` | send that analysis (the top bar's `send` gates both) |
| `graph` | plot that analysis against time, under the grid or the faders |
| `+AF3/4` | average AF3/AF4 into both emotion indices, for stability |
| `smooth` | low-pass cutoff, per analysis; the readout gives the time constant, which is the number worth reading |
| `range` / `focus` | ln units at full deflection — 0.50 means e^0.5, a factor of 1.6 in the underlying ratio |
| `relax` | the multiple of baseline occipital alpha that reads as a full fader, and where the fader counts as "eyes closed" |
| `baseline` | how long the rolling baseline remembers, and the span of the graphs; shared by both analyses |
| `calibrate` | freeze the baseline at the average of the next 10 s |
| `osc` rate | upper bound on how often values go out; shared |

Changing `+AF3/4` restarts both emotion indices and drops the history —
the index means something different afterwards, and carrying the old samples
across would put a step in the trail that never happened. Changing a range
does not: the history is stored as deviations and mapped when drawn, so a
range slider re-scales what is already on screen.

The four values go out as

```
<prefix>/pad/valence   -1 .. +1
<prefix>/pad/arousal   -1 .. +1
<prefix>/cog/focus      0 .. 1, resting at 0.5
<prefix>/cog/relax      0 .. 1, resting at 0
```

all as OSC floats, with the whole set in one batch so the values describing
one instant travel together. The `osc` rate is an upper bound, not a
promise: each batch is one POST from the browser, and the round trip is part
of every cycle. The cognitive header reports the rate actually achieved —
about 7 Hz against a 10 Hz setting in the headless Chrome used for testing.

The analysis runs in the page, so it keeps sending with the widget closed
but not with the tab closed, and pausing the viewer stops it. Nothing is
emitted while the values are frozen: repeating a stale reading ten times a
second would be a lie of exactly the kind this project avoids elsewhere.

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

The backend emits OSC of its own for the widgets that derive something. The
analysis tool and the gyro's estimated position use it; the endpoints are
open to anything else that wants them. Each widget rate-limits its own
stream and keeps one request in flight at a time, so a slow reply cannot
build a queue of readings that are stale by the time they leave.

```
POST /api/output  {"enabled": true, "host": "127.0.0.1", "port": 9100}
POST /api/emit    {"address": "focus", "args": [0.42]}   -> /xavier/focus
POST /api/emit    {"messages": [{"address": "cog/focus", "args": [0.42]},
                                {"address": "cog/relax", "args": [0.1]}]}
```

A batch is sent under one lock, so a set of values describing one instant
cannot straddle a change of endpoint or arrive interleaved with another
tool's. The reply is `{"sent": n, "requested": n, "output": {...}}`; `sent`
is zero, not an error, when output is switched off.

**Numbers go out as OSC floats.** JSON has one number type, so a value that
happens to land exactly on zero would otherwise be tagged `i` while every
other sample is tagged `f` — which breaks a receiver an hour into a run, on
the one value most likely to occur.

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
