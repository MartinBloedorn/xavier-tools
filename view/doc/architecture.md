# xavier-viewer — internals

How the viewer is put together, and why. User-facing instructions are in the
[README](../README.md); this covers the decisions and the parts that would
otherwise have to be re-derived.

## Layout

```
view/
  xavier-viewer           entry point (also: python -m viewer)
  xavier-viewer.cmd       Windows wrapper
  viewer.conf.json        created on first run; the whole persisted state
  viewer/
    app.py                argument parsing, wiring, the startup banner
    config.py             viewer.conf.json: defaults, deep merge, atomic write
    osc.py                OSC 1.0 decode (and encode, for output)
    receiver.py           the UDP thread: bind, decode, route into the hub
    state.py              ring buffers, fan-out to browsers, link statistics
    sender.py             outbound OSC, for analysis tools
    server.py             HTTP: static assets, the SSE stream, the control API
    web/
      index.html          the shell
      style.css           tokens and every rule
      app.js              top bar, widget layout, dividers, the frame loop
      lib/ring.js         typed-array ring buffers
      lib/plot.js         canvas primitives: axes, ticks, palette, smoothing
      lib/stream.js       the SSE client and the config store
      lib/controls.js     the controls that live in a parameter bar
      widgets/data.js     the data viewer
      widgets/gyro.js     the gyro viewer
  tools/
    fake_dat.py           synthetic EPOC over real UDP
    cdp.py                headless Chrome over the DevTools protocol
```

## Dependencies: none, on both sides

Stock Python, no pip install. Vanilla JS, no CDN, no bundler, no build step.

This matches the rest of the project — the C++ tool hand-rolls AES and OSC
rather than take a dependency — but it is not only consistency. The viewer is
a diagnostic tool for hardware that is already discontinued; a pip
environment that has rotted, or a CDN that 404s, is a worse failure mode
than the code it would have saved.

The charting is the part where this most deserves scrutiny, and it is also
where it pays best. Fourteen stacked realtime traces, each with a second
independently scaled bar chart overlaid and four axes between them, is not a
shape general charting libraries do well; the ones that re-layout per frame
do not keep up at 60 fps at all. `lib/plot.js` is about 200 lines.

## The data path

```
epoc --osc  ->  UDP  ->  receiver thread  ->  Hub  ->  SSE  ->  browser
```

**`receiver.py`** owns one socket and one thread. `recvfrom` with a 250 ms
timeout, so a stop or a re-point is noticed promptly rather than after the
next datagram — which, if the headset is off, may be never.

**`state.py`** is the only shared mutable state, under one lock. Python's
GIL makes finer-grained locking pointless here.

**`server.py`** serves each browser from its own thread.

### Fan-out is push, not cursor

The hub appends each sample to every subscriber's own bounded deque, rather
than keeping one shared ring that subscribers read with cursors. With one or
two viewers that is a couple of hundred list appends a second and no cursor
arithmetic; a subscriber that stops draining — a backgrounded tab, a stalled
socket — sheds its oldest samples to `maxlen` instead of growing without
limit.

A new subscriber is primed with the recent history, so a freshly opened page
shows a populated graph rather than several seconds of blank axes.

### Pacing is by clock, not by arrival

This is the one thing here that was got wrong first and is worth recording.

The obvious design is an event the receiver sets and the connection thread
waits on. At 128 Hz that delivers **one sample per wake-up**: 128 events a
second, each carrying a single frame and a full status block beside it —
more events per second than any display can use, at several times the bytes.

Measured, before and after:

| | events/s | frames/event | bytes/s |
|---|---|---|---|
| wake on arrival | 121 | 1.0 | ~76 k |
| 40 Hz clock | 39 | 3.2 | ~45 k |

So the connection thread paces itself off `TICK_SECONDS` and drains whatever
has accumulated. There is deliberately no "data ready" event to wait on; the
`Subscriber` docstring says so, because re-adding one looks like an
improvement.

40 Hz is below one frame at 60 Hz, which is the point — the display cannot
show more.

### SSE, not WebSocket

Data only ever flows one way. SSE needs no framing layer, the browser
reconnects on its own, and `EventSource` is four lines. Control goes the
other way as ordinary JSON POSTs, which is also how the analysis tools will
ask for OSC to be emitted.

The cost is one detail: the response has no `Content-Length` and no chunking,
so the body is delimited by the close. That is valid HTTP/1.1 framing and
what every browser implements for SSE, but it does mean the connection
cannot be kept alive — hence the explicit `Connection: close`.

It also means **the page never reaches "load finished"**, which is why
`tools/cdp.py` exists: `chrome --screenshot` waits for that event and so
hangs forever on this page.

### Status is unconditional

Every payload carries the link status, and one is sent at least every 250 ms
even with no data at all. The case where the status matters most is exactly
the case where no data is arriving.

`state` is `waiting` (nothing ever seen), `live`, or `stalled` (nothing for
1.5 s). A stalled rate is reported as zero rather than as the last measured
figure: "128 Hz" beside a red dot would be actively misleading.

### Prefix diagnostics

The single most common reason for an empty screen is a prefix that does not
match what the acquisition tool is sending, and UDP will not tell you. The
hub counts well-formed OSC whose address is not under the configured prefix
and records the address roots it saw, capped at eight so a chatty network
cannot grow it without bound. The browser turns that into
*"OSC arriving on /epoc — prefix mismatch?"*.

This is the reason the receiver does not simply drop non-matching traffic.

## OSC decoding

`osc.py` is the mirror of `epoc/src/osc.cpp` — the receive half that the C++
side deliberately never implemented. The two byte-layout rules are the same
ones documented in [epoc/doc/osc.md](../../epoc/doc/osc.md): strings are
NUL-terminated and padded to a multiple of four with *always at least one*
terminator, and all numeric arguments are big-endian.

Bundles are decoded even though the DAT never sends them. Fifteen lines, and
it means a receiver sitting behind some other OSC router still works.

### Finding the timestamp

`--osc-timestamp` has four settings and only one of them is
self-describing. So the timestamp is found by **arity**, not by position: an
address whose payload should be 14 values, arriving with 15, has a timestamp
in front. A message with neither shape is ignored rather than guessed at —
it means some other tool is using the prefix.

The value is then interpreted by type: a `TimeTag` converts exactly; a
number above 1e11 is epoch milliseconds (`int64` or `double`); anything
smaller is `int32` milliseconds-since-DAT-start, which has no relationship
to this machine's clock, so arrival time is used. Those two cases are nine
orders of magnitude apart, so the test is not close to ambiguous.

### Re-pairing the gyro

The DAT sends `/gyro/x` and `/gyro/y` as separate messages so a receiver can
route the axes independently. The viewer wants them back as pairs, and the
matching key is the timestamp: one timestamp is taken per sample and shared
by every message describing it, so the happy path is an equality test on
floats that came from the same integer. `_on_gyro` carries a sticky
last-known value for the fallbacks, because a lost datagram should cost one
axis of one sample rather than the sample.

## The browser

### Rendering

One canvas per widget, redrawn whole each frame from `requestAnimationFrame`.
No diffing, no retained scene graph — at fourteen traces of ~1300 points the
whole frame is cheaper than working out what changed.

Samples live in `SampleRing`: one shared `Float64Array` of timestamps and N
parallel `Float32Array` tracks. One timestamp array because every channel in
a `/raw/all` message shares one — storing it fourteen times would triple the
memory for nothing. Typed arrays because the draw loop walks most of the
buffer every frame, and object allocation at that rate is what makes canvas
plotting stutter.

**Min/max decimation.** When a window holds more samples than the plot has
pixels, the trace is drawn as a per-column envelope rather than by striding.
Stride sampling would alias a 10 Hz rhythm into whatever beat frequency it
makes with the column width — a plausible-looking trace of something that is
not there. Min/max keeps the envelope honest.

**The right-hand edge is eased.** Batches arrive at 40 Hz and the display
runs at 60, so following the newest sample directly advances the trace in
visible steps. `displayTime` chases it at 0.3 per frame, snapping if the gap
exceeds a second.

**Auto-ranges are smoothed asymmetrically** (`Smoothed` in `lib/plot.js`):
fast to grow, slow to shrink. Scaling straight off each frame's extremes
makes a trace jitter vertically; a symmetric smoother makes a sudden artefact
clip for a second before the axis catches up.

### Colour

The palette lives in CSS custom properties and the canvas reads it back
through `getComputedStyle`, so there is one source for both halves.

Dark only, deliberately. This is a scope; a light theme would wash out the
traces, and the widget that matters most is a canvas whose palette would then
have to be maintained twice.

The trace/bar contrast asked for in the planning note is carried by weight
rather than hue alone: traces are fully saturated and drawn over the bars,
which are a 5–24% amber wash with a brighter top edge. Channels get a gentle
cyan-to-green ramp — narrow on purpose, since the point is to help the eye
follow a row across the stack, not to identify a channel by colour. The
labels do that, and fourteen maximally distinct hues would look like a bag
of sweets.

### State

The browser owns the `ui` subtree of the config and posts it back on every
change, debounced 400 ms — a slider drag would otherwise be a few hundred
POSTs and a few hundred rewrites of the file. `pagehide` flushes through
`sendBeacon`, because a debounce loses the last change when the tab closes.

The backend does not validate the `ui` blob. It is the client's own state,
and a backend that second-guessed its shape would need editing every time a
widget grew a checkbox.

Widgets are **all instantiated at startup**, open or not, and all of them
ingest. One opened later then already has history behind it instead of
drawing a blank window for the next several seconds.

### Bubble layout

The gyro's bubble is square and the widget rarely is, so a wide one has
horizontal room to spare; the numbers go in it. The room they need is
reserved on **both** sides, which keeps the bubble centred in the widget: a
spirit level that sits off to one side, and slides sideways the moment the
readout appears, is reporting its own layout rather than the position of the
head.

In the band of widths where the bubble cannot be both full size and flanked
by numbers, it gives up radius rather than the numbers — but only down to
three quarters of its unencumbered size, below which the readout is dropped
instead. The gutter is measured from the actual text (`readoutSpan`), not
guessed: it sets the bubble's size as well as deciding whether the readout
appears at all, and a constant would either waste radius or clip a digit
depending on the font the browser picked.

### Gyro sign convention

Which way "up" is depends on how the headset sits on a particular head, and
the protocol does not pin it down -- the zero offsets in the decoder are
emokit's, inherited without justification. So the gyro widget has a `flip`
toggle per axis, and the sign is applied **once, at ingest**: the ring holds
rates already in the chosen orientation, and the bubble, trail, readout,
graph and auto-scales need no idea the setting exists.

The cost of storing it signed is that flipping has to be applied to history
too -- `invert()` negates the stored rate track and the zero offset, then
recomputes the angles. Flipping only samples yet to arrive would put a step
in the middle of the graph at the moment of the click, which reads as a
movement that never happened.

One toggle per axis, not per series: yaw is the integral of x, so they flip
together. A rate going one way and the angle derived from it going the other
is a display that contradicts itself.

### Pause

Pausing stops ingestion into the widgets, not the stream. Status and battery
keep updating, because a frozen connection indicator would be a lie — and
"is it paused or is it dead?" is precisely the question the indicator exists
to answer.

### Widget contract

Four methods: `buildBar(bar)`, `ingest(msg)`, `render(canvas, {paused})`,
`reset()`. `app.js` owns the DOM around a widget and the clock; a widget owns
its buffers and its canvas and nothing else. Adding one is: write the class,
add it to `WIDGETS`, add a button with `data-widget="<id>"`, and add a
defaults block in `config.py`.

Widths are flex-grow weights, so they survive a window resize as
proportions. The minimum is a sixth of the *window*, per the spec — not a
sixth of the pair — so a third widget cannot be crushed by a drag between
the other two. When a pair is itself narrower than two floors, it splits
evenly rather than letting the two clamps fight.

A parameter bar is two rows. `buildBar` appends `ui.row()` containers and
puts its groups in those, never in the bar itself; the fixed row count is
what makes "one height for all widgets" structural rather than a convention
each widget has to remember. A single CSS variable sets the row height, and
the collapse chevron toggles every bar together.

One row was the first attempt and it overflowed at any realistic widget
width — the data viewer alone has fourteen channel chips before the first
parameter. A control you have to scroll sideways to reach may as well not be
on the bar, so the split is by kind: channel selection on the data widget's
top row and everything else below; what is on screen and over what span on
the gyro's top row, with the integrator's own parameters below.

Rows still scroll individually if one overflows anyway. Within a row, every
group keeps its natural width rather than shrinking: letting the widest group
shrink was tried, and it always picked the channel toggles — the controls
most worth being able to reach.

## Testing

There is no unit-test suite. What there is instead:

`tools/fake_dat.py` drives the whole receive path over real UDP with real
OSC bytes. Its band powers come from a real FFT with the same normalisation
as `dsp.cpp`, which makes them a check on the display rather than just
filler: a 14 µV alpha rhythm reads as ~98 µV², its closed-form A²/2, on the
bars. If the band axis is ever wrong, this shows it.

`tools/cdp.py` drives the page. It speaks enough RFC 6455 to carry JSON over
the DevTools WebSocket, and it is what verified the interactive behaviour:
the 1/6 resize floor (at a 1572 px window, both clamps landed on 262 px),
bar heights staying equal across widgets, pause actually freezing ingestion,
prefix-mismatch and dead-port recovery, and a clean console throughout.

This is a gap worth naming: there is nothing that would catch a regression
automatically. The OSC codec in particular is pure and would be easy to pin.

## Open items

- **No automated tests.** See above. `osc.py` is the obvious first target.
- **Never run against real hardware.** Everything has been verified against
  `fake_dat.py`. The synthetic stream is a faithful reproduction of the
  message set, rates and encodings, but it is not a headset.
- **The analysis-tool output path has no analysis tools.** `POST /api/emit`
  works and is tested; nothing calls it.
- **Only tested in Chrome.** Nothing used is Chrome-specific —
  `EventSource`, canvas 2D, ES modules, pointer events — but Firefox and
  Safari have not been opened.
- **One viewer at a time is the assumed case.** Several browsers can connect
  and each gets its own subscriber, but they share one `viewer.conf.json` and
  will overwrite each other's layout.
