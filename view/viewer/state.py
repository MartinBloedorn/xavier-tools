"""Shared stream state: ring buffers, fan-out to browsers, link statistics.

One :class:`Hub` sits between the UDP receiver thread and however many
browser connections there are. The receiver pushes samples in; each browser
holds a :class:`Subscriber` and drains its own copy out.

Fan-out is push-based -- the receiver appends to every subscriber's queue --
rather than cursor-based over a single shared ring. With one or two viewers
that costs a couple of hundred list appends a second and needs no cursor
arithmetic; a subscriber that stops draining loses its oldest samples to a
bounded deque rather than growing without limit.
"""

from __future__ import annotations

import threading
import time
from collections import deque
from typing import Any, Deque, Dict, List, Optional, Sequence, Tuple

# Channel order, matching `epoc::kChannels`. This ordering is load-bearing:
# /raw/all and /quality/all are positional, so it has to agree with the
# headset's left-to-right convention used everywhere in the C++ tool.
CHANNELS: Tuple[str, ...] = (
    "AF3", "F7", "F3", "FC5", "T7", "P7", "O1",
    "O2", "P8", "T8", "FC6", "F4", "F8", "AF4",
)
CHANNEL_COUNT = len(CHANNELS)

# Band edges from epoc/doc/dsp.md, half-open [low, high).
BANDS: Tuple[Tuple[str, float, float], ...] = (
    ("delta", 0.5, 4.0),
    ("theta", 4.0, 8.0),
    ("alpha", 8.0, 13.0),
    ("beta", 13.0, 30.0),
    ("gamma", 30.0, 45.0),
)

# `epoc::kMicrovoltsPerCount`. Nominal, not calibrated per headset -- the
# conversion happens in the viewer precisely because it is indicative.
MICROVOLTS_PER_COUNT = 0.51
NOMINAL_SAMPLE_RATE = 128.0

# emokit treats > 4000 as a good contact; the CLI uses it as bar full-scale.
# A display convention, not a calibrated threshold.
QUALITY_FULL_SCALE = 4000

# How much history to keep for backfilling a browser that has just connected,
# so a freshly opened page shows a populated graph instead of blank axes.
HISTORY_SECONDS = 20.0
_HISTORY_FRAMES = int(HISTORY_SECONDS * NOMINAL_SAMPLE_RATE)

# A subscriber that stops draining -- a backgrounded tab, a stalled socket --
# keeps at most this much before it starts shedding its oldest samples.
_QUEUE_FRAMES = int(4.0 * NOMINAL_SAMPLE_RATE)

# No packet for this long and the link is reported as stalled rather than
# live. Comfortably longer than any gap the DAT produces at 128 Hz.
STALL_SECONDS = 1.5


def _round(value: float, digits: int = 4) -> float:
    return round(value, digits)


class Subscriber:
    """One browser's view of the stream.

    Everything here is touched by two threads: the receiver appends, the
    connection's own thread drains. The hub's lock covers both.

    There is deliberately no "data ready" event to wait on. Draining is
    paced by the connection's own clock, because a wake-on-arrival loop at
    128 Hz just delivers one sample per wake-up -- far more events per
    second than any display can use, each carrying a single frame and a
    full status block alongside it.
    """

    __slots__ = ("raw", "gyro", "quality_dirty", "fft_dirty", "battery_dirty")

    def __init__(self) -> None:
        self.raw: Deque[Tuple[float, Sequence[int]]] = deque(maxlen=_QUEUE_FRAMES)
        self.gyro: Deque[Tuple[float, int, int]] = deque(maxlen=_QUEUE_FRAMES)
        # Latest-value streams: no queue, just a flag saying the browser has
        # not yet seen the current value. Sending every /quality/all at 34 Hz
        # when the browser repaints at 60 would be pure repetition.
        self.quality_dirty = True
        self.fft_dirty = True
        self.battery_dirty = True


class Hub:
    """Stream state plus the fan-out to subscribers."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._subscribers: List[Subscriber] = []

        # Time origin for everything sent to the browser. Absolute UNIX
        # seconds are ~1.8e9 and would cost four significant digits of
        # float precision in JSON for no benefit; the browser only ever
        # needs differences.
        self.t0 = time.time()

        self._raw: Deque[Tuple[float, Sequence[int]]] = deque(maxlen=_HISTORY_FRAMES)
        self._gyro: Deque[Tuple[float, int, int]] = deque(maxlen=_HISTORY_FRAMES)
        self._quality: Optional[List[int]] = None
        self._fft: List[Optional[List[float]]] = [None] * CHANNEL_COUNT
        self._battery: Optional[float] = None

        # Link statistics. `_arrivals` holds local receive times, not stream
        # timestamps, so the measured rate reflects what is actually
        # arriving over the wire rather than what the DAT believes it sent.
        self._arrivals: Deque[float] = deque(maxlen=256)
        self._packets = 0
        self._samples = 0
        self._last_seen = 0.0
        self._first_seen = 0.0
        self._malformed = 0
        # Address roots seen that did not match the configured prefix. The
        # single most common way to see nothing is a prefix mismatch, and
        # without this the symptom is an empty screen with no explanation.
        self._foreign: Dict[str, int] = {}
        self._unmatched = 0

    # -- subscriber management --------------------------------------------

    def subscribe(self) -> Subscriber:
        """Register a browser, primed with the recent history."""
        sub = Subscriber()
        with self._lock:
            # deque(maxlen=) on the subscriber will keep only the tail if
            # history is longer than the queue, which is the right loss.
            sub.raw.extend(self._raw)
            sub.gyro.extend(self._gyro)
            self._subscribers.append(sub)
        return sub

    def unsubscribe(self, sub: Subscriber) -> None:
        with self._lock:
            try:
                self._subscribers.remove(sub)
            except ValueError:
                pass

    @property
    def subscriber_count(self) -> int:
        with self._lock:
            return len(self._subscribers)

    # -- ingest -----------------------------------------------------------

    def note_packet(self, now: float) -> None:
        """Record the arrival of a matching packet, for the rate display."""
        with self._lock:
            self._packets += 1
            self._last_seen = now
            if not self._first_seen:
                self._first_seen = now

    def note_malformed(self) -> None:
        with self._lock:
            self._malformed += 1

    def note_foreign(self, address: str) -> None:
        """A well-formed OSC message whose address is not under our prefix."""
        root = "/" + address.lstrip("/").split("/", 1)[0]
        with self._lock:
            self._unmatched += 1
            if root not in self._foreign and len(self._foreign) >= 8:
                return  # do not let a chatty network grow this without limit
            self._foreign[root] = self._foreign.get(root, 0) + 1

    def push_raw(self, t: float, counts: Sequence[int], now: float) -> None:
        item = (t, counts)
        with self._lock:
            self._samples += 1
            self._arrivals.append(now)
            self._raw.append(item)
            for sub in self._subscribers:
                sub.raw.append(item)

    def push_gyro(self, t: float, x: int, y: int) -> None:
        item = (t, x, y)
        with self._lock:
            self._gyro.append(item)
            for sub in self._subscribers:
                sub.gyro.append(item)

    def push_quality(self, values: Sequence[int]) -> None:
        with self._lock:
            self._quality = list(values)
            for sub in self._subscribers:
                sub.quality_dirty = True

    def push_fft(self, channel: int, bands: Sequence[float]) -> None:
        if not 0 <= channel < CHANNEL_COUNT:
            return
        with self._lock:
            self._fft[channel] = [float(v) for v in bands]
            for sub in self._subscribers:
                sub.fft_dirty = True

    def push_battery(self, level: float) -> None:
        with self._lock:
            self._battery = float(level)
            for sub in self._subscribers:
                sub.battery_dirty = True

    def reset_stream(self) -> None:
        """Forget everything. Called when the OSC endpoint is re-pointed --
        the old buffers describe a different stream."""
        with self._lock:
            self._raw.clear()
            self._gyro.clear()
            self._quality = None
            self._fft = [None] * CHANNEL_COUNT
            self._battery = None
            self._arrivals.clear()
            self._packets = 0
            self._samples = 0
            self._last_seen = 0.0
            self._first_seen = 0.0
            self._malformed = 0
            self._foreign = {}
            self._unmatched = 0
            for sub in self._subscribers:
                sub.raw.clear()
                sub.gyro.clear()
                sub.quality_dirty = True
                sub.fft_dirty = True
                sub.battery_dirty = True

    # -- egress -----------------------------------------------------------

    def status(self, now: Optional[float] = None) -> Dict[str, Any]:
        now = time.time() if now is None else now
        with self._lock:
            return self._status_locked(now)

    def _status_locked(self, now: float) -> Dict[str, Any]:
        rate = 0.0
        if len(self._arrivals) >= 2:
            span = self._arrivals[-1] - self._arrivals[0]
            if span > 0:
                rate = (len(self._arrivals) - 1) / span
        if not self._last_seen:
            state = "waiting"
        elif now - self._last_seen > STALL_SECONDS:
            state = "stalled"
        else:
            state = "live"
        # A stalled rate is a stale rate; showing 128 Hz next to a red dot
        # would be actively misleading.
        if state != "live":
            rate = 0.0
        return {
            "state": state,
            "rate": _round(rate, 1),
            "packets": self._packets,
            "samples": self._samples,
            "malformed": self._malformed,
            "unmatched": self._unmatched,
            "foreign": sorted(self._foreign.keys()),
            "battery": None if self._battery is None else _round(self._battery, 4),
            "uptime": _round(now - self._first_seen, 1) if self._first_seen else 0.0,
            "since": _round(now - self._last_seen, 2) if self._last_seen else None,
        }

    def drain(self, sub: Subscriber, now: Optional[float] = None) -> Dict[str, Any]:
        """Collect everything *sub* has not yet been sent.

        Always returns a payload -- ``status`` is unconditional, so the
        browser's connection indicator keeps updating even when no data is
        arriving at all, which is exactly the case it most needs to show.
        """
        now = time.time() if now is None else now
        t0 = self.t0
        with self._lock:
            raw = list(sub.raw)
            sub.raw.clear()
            gyro = list(sub.gyro)
            sub.gyro.clear()
            quality = self._quality if (sub.quality_dirty and self._quality) else None
            sub.quality_dirty = False
            fft: Optional[List[Optional[List[float]]]] = None
            if sub.fft_dirty:
                fft = list(self._fft)
            sub.fft_dirty = False
            battery = self._battery if sub.battery_dirty else None
            sub.battery_dirty = False
            payload: Dict[str, Any] = {"status": self._status_locked(now)}

        if raw:
            payload["raw"] = {
                "t": [_round(t - t0) for t, _ in raw],
                "v": [list(v) for _, v in raw],
            }
        if gyro:
            payload["gyro"] = {
                "t": [_round(g[0] - t0) for g in gyro],
                "x": [g[1] for g in gyro],
                "y": [g[2] for g in gyro],
            }
        if quality is not None:
            payload["quality"] = quality
        if fft is not None and any(v is not None for v in fft):
            # Band powers span orders of magnitude, so trim to significant
            # figures rather than decimal places: %.6g keeps 1e-4 and 1e4
            # equally well, round(x, 4) destroys the former.
            payload["fft"] = [
                None if v is None else [float("{:.6g}".format(p)) for p in v]
                for v in fft
            ]
        if battery is not None:
            payload["battery"] = battery
        return payload
