"""The UDP side: bind a socket, decode OSC, route it into the :class:`Hub`.

Runs on its own thread. The socket can be re-pointed at a different port or
prefix while running -- the browser exposes both in the top bar, and having
to restart the viewer to correct a typo would be tedious.
"""

from __future__ import annotations

import socket
import threading
import time
from typing import Any, List, Optional, Sequence, Tuple

from . import osc
from .state import CHANNELS, CHANNEL_COUNT, Hub

# Channel name (lowercased, as it appears in the address) -> index.
# Addresses are lowercased by the C++ side because OSC address patterns are
# case-sensitive; the display casing is a separate concern.
_CHANNEL_INDEX = {name.lower(): i for i, name in enumerate(CHANNELS)}

# Expected argument count *excluding* the timestamp, per address suffix.
# Used to decide whether a leading timestamp is present rather than assuming
# it: --osc-messages without `t` is a legal thing for a user to do.
_ARITY = {
    "/raw/all": CHANNEL_COUNT,
    "/quality/all": CHANNEL_COUNT,
    "/gyro/x": 1,
    "/gyro/y": 1,
    "/battery": 1,
}
_FFT_ARITY = 5

# Epoch milliseconds are ~1.8e12 now and will not drop below this in any
# plausible future; `--osc-timestamp int32` sends milliseconds since stream
# start, which is far smaller. The gap is nine orders of magnitude wide, so
# this test is not close to ambiguous.
_EPOCH_MS_FLOOR = 1e11


class Receiver:
    """Owns the UDP socket and the decode loop."""

    def __init__(self, hub: Hub, bind: str, port: int, prefix: str,
                 verbose: bool = False) -> None:
        self.hub = hub
        self.verbose = verbose
        self._lock = threading.Lock()
        self._bind = bind
        self._port = int(port)
        self._prefix = osc.normalize_prefix(prefix)
        self._sock: Optional[socket.socket] = None
        self._want_rebind = True
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self.last_error: Optional[str] = None

        # Gyro arrives as two single-argument messages sharing a timestamp,
        # so that a receiver can route the axes independently. We want them
        # back as pairs; see _on_gyro.
        self._pending_x: Optional[Tuple[float, int]] = None
        self._last_x = 0
        self._last_y = 0

    # -- lifecycle --------------------------------------------------------

    @property
    def endpoint(self) -> Tuple[str, int, str]:
        with self._lock:
            return self._bind, self._port, self._prefix

    def start(self) -> None:
        self._thread = threading.Thread(target=self._run, name="osc-rx", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        with self._lock:
            sock, self._sock = self._sock, None
        if sock is not None:
            try:
                sock.close()
            except OSError:
                pass
        if self._thread is not None:
            self._thread.join(timeout=2.0)

    def repoint(self, bind: Optional[str] = None, port: Optional[int] = None,
                prefix: Optional[str] = None) -> None:
        """Change endpoint or prefix while running.

        A prefix change alone does not need a new socket, but it does
        invalidate every buffer -- what was being displayed came from a
        different stream -- so both paths clear the hub.
        """
        with self._lock:
            rebind = False
            if bind is not None and bind != self._bind:
                self._bind, rebind = bind, True
            if port is not None and int(port) != self._port:
                self._port, rebind = int(port), True
            if prefix is not None:
                normalized = osc.normalize_prefix(prefix)
                if normalized != self._prefix:
                    self._prefix = normalized
            if rebind:
                self._want_rebind = True
                sock, self._sock = self._sock, None
        if rebind and sock is not None:
            try:
                sock.close()  # unblocks recvfrom immediately
            except OSError:
                pass
        self.hub.reset_stream()

    # -- the loop ---------------------------------------------------------

    def _open(self) -> Optional[socket.socket]:
        with self._lock:
            bind, port = self._bind, self._port
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            # A 128 Hz stream of small datagrams fills the default buffer
            # quickly if this thread is ever descheduled; ask for more and
            # do not care if the OS declines.
            try:
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
            except OSError:
                pass
            sock.bind((bind, port))
            sock.settimeout(0.25)  # so stop/rebind is noticed promptly
        except OSError as exc:
            self.last_error = "cannot bind {}:{} -- {}".format(bind, port, exc)
            print("xavier-viewer: {}".format(self.last_error))
            return None
        self.last_error = None
        print("xavier-viewer: listening for OSC on {}:{}".format(bind, port))
        return sock

    def _run(self) -> None:
        while not self._stop.is_set():
            with self._lock:
                sock = self._sock
                needs_open = self._want_rebind or sock is None
            if needs_open:
                new_sock = self._open()
                with self._lock:
                    self._want_rebind = False
                    self._sock = new_sock
                if new_sock is None:
                    # Bind failed -- almost always the port being in use.
                    # Retry slowly rather than spinning; the user may free
                    # it, or re-point us somewhere else from the UI.
                    self._stop.wait(2.0)
                continue

            try:
                data, _addr = sock.recvfrom(65535)
            except socket.timeout:
                continue
            except OSError:
                # Closed underneath us by stop() or repoint().
                with self._lock:
                    if self._sock is sock:
                        self._sock = None
                continue

            now = time.time()
            try:
                messages = osc.decode_packet(data)
            except osc.OscError:
                self.hub.note_malformed()
                continue
            for address, args in messages:
                self._dispatch(address, args, now)

    # -- routing ----------------------------------------------------------

    def _dispatch(self, address: str, args: List[Any], now: float) -> None:
        with self._lock:
            prefix = self._prefix
        if prefix == "/":
            suffix = address if address.startswith("/") else "/" + address
        elif address.startswith(prefix + "/"):
            suffix = address[len(prefix):]
        elif address == prefix:
            suffix = "/"
        else:
            self.hub.note_foreign(address)
            return

        if suffix == "/raw/all":
            values = self._payload(args, _ARITY[suffix])
            if values is None:
                return
            t, rest = values
            self.hub.note_packet(now)
            self.hub.push_raw(t, [int(v) for v in rest], now)
        elif suffix == "/quality/all":
            values = self._payload(args, _ARITY[suffix])
            if values is None:
                return
            self.hub.note_packet(now)
            self.hub.push_quality([int(v) for v in values[1]])
        elif suffix in ("/gyro/x", "/gyro/y"):
            values = self._payload(args, 1)
            if values is None:
                return
            self.hub.note_packet(now)
            self._on_gyro(suffix.endswith("x"), values[0], int(values[1][0]))
        elif suffix == "/battery":
            values = self._payload(args, 1)
            if values is None:
                return
            self.hub.note_packet(now)
            self.hub.push_battery(float(values[1][0]))
        elif suffix.startswith("/fft/"):
            index = _CHANNEL_INDEX.get(suffix[5:])
            if index is None:
                return
            values = self._payload(args, _FFT_ARITY)
            if values is None:
                return
            self.hub.note_packet(now)
            self.hub.push_fft(index, [float(v) for v in values[1]])
        elif suffix.startswith("/raw/"):
            # Per-channel raw ('r'). We ask for 'u' instead, which carries
            # the same data in a fourteenth of the packets -- but say so
            # rather than counting it as foreign traffic.
            self.hub.note_packet(now)
        # Anything else under our own prefix is simply not displayed.

    def _payload(self, args: Sequence[Any], arity: int
                 ) -> Optional[Tuple[float, Sequence[Any]]]:
        """Split a leading timestamp off *args* and validate the arity.

        Returns ``(unix_seconds, values)``, or ``None`` if the message does
        not have the shape this address is supposed to have -- which means
        some other tool is using our prefix, and guessing would be worse
        than ignoring it.
        """
        if len(args) == arity + 1:
            return self._timestamp(args[0]), args[1:]
        if len(args) == arity:
            # No `t` flag on the DAT. Local arrival time is then the best
            # available, and is good enough for a live display.
            return time.time(), args
        return None

    def _timestamp(self, value: Any) -> float:
        if isinstance(value, osc.TimeTag):
            return value.unix
        if isinstance(value, (int, float)) and not isinstance(value, bool):
            if abs(value) >= _EPOCH_MS_FLOOR:
                return float(value) / 1000.0  # int64/double epoch milliseconds
            # `--osc-timestamp int32`: milliseconds since the DAT started,
            # which has no relationship to our clock. Arrival time it is.
            return time.time()
        return time.time()

    def _on_gyro(self, is_x: bool, t: float, value: int) -> None:
        """Re-pair the two single-axis messages into one sample.

        The DAT sends /gyro/x then /gyro/y back to back with an identical
        timestamp -- one timestamp is taken per sample and shared by every
        message describing it -- so the matching path is an equality test on
        floats that came from the same integer. The fallbacks cover a lost
        datagram, which UDP will not tell us about.
        """
        if is_x:
            if self._pending_x is not None:
                # Previous x never saw its y. Emit it with the last known y
                # rather than dropping the sample.
                prev_t, prev_x = self._pending_x
                self.hub.push_gyro(prev_t, prev_x, self._last_y)
            self._pending_x = (t, value)
            self._last_x = value
            return

        self._last_y = value
        if self._pending_x is not None and self._pending_x[0] == t:
            self.hub.push_gyro(t, self._pending_x[1], value)
            self._pending_x = None
        else:
            if self._pending_x is not None:
                prev_t, prev_x = self._pending_x
                self.hub.push_gyro(prev_t, prev_x, self._last_y)
                self._pending_x = None
            self.hub.push_gyro(t, self._last_x, value)
