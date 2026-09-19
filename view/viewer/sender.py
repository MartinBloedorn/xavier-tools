"""Outbound OSC: what the analysis tools produce.

The analysis widget computes in the browser -- that is where the band powers
already are -- and posts its results to ``/api/emit``, which lands here.

Mirrors the C++ sender's posture: connectionless UDP, failed sends counted
rather than thrown. A dropped datagram must not take down a live session.
"""

from __future__ import annotations

import socket
import threading
from typing import Any, Iterable, Optional, Sequence, Tuple

from . import osc


class Sender:
    """UDP OSC output to one destination, re-pointable at runtime."""

    def __init__(self, host: str = "127.0.0.1", port: int = 9100,
                 prefix: str = "/xavier", enabled: bool = False) -> None:
        self._lock = threading.Lock()
        self._host = host
        self._port = int(port)
        self._prefix = osc.normalize_prefix(prefix)
        self._enabled = bool(enabled)
        self._sock: Optional[socket.socket] = None
        self.sent = 0
        self.failed = 0
        self.last_error: Optional[str] = None

    def configure(self, host: Optional[str] = None, port: Optional[int] = None,
                  prefix: Optional[str] = None, enabled: Optional[bool] = None) -> None:
        with self._lock:
            if host is not None:
                self._host = host
            if port is not None:
                self._port = int(port)
            if prefix is not None:
                self._prefix = osc.normalize_prefix(prefix)
            if enabled is not None:
                self._enabled = bool(enabled)

    def _socket(self) -> socket.socket:
        if self._sock is None:
            self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        return self._sock

    def send(self, address: str, args: Sequence[Any] = ()) -> bool:
        """Send one message. *address* is joined to the configured prefix
        unless it is already absolute under it."""
        return self.send_many([(address, args)]) == 1

    def send_many(self, messages: Iterable[Tuple[str, Sequence[Any]]]) -> int:
        """Send several messages, returning how many left the socket.

        One call, one lock, one destination read: an analysis tool produces
        its whole result at once -- valence and arousal describe the same
        instant -- and they should not be able to straddle a change of
        endpoint, nor arrive with another tool's readings interleaved.

        A failure is counted and the rest are still attempted. Half a result
        set is more useful than none, and the alternative is that one
        malformed argument silently stops a live installation's stream.
        """
        sent = 0
        with self._lock:
            if not self._enabled:
                return 0
            prefix, host, port = self._prefix, self._host, self._port
            for address, args in messages:
                if address.startswith(prefix + "/") or address == prefix:
                    full = address
                else:
                    leaf = address.lstrip("/")
                    full = leaf and (prefix.rstrip("/") + "/" + leaf) or prefix
                try:
                    packet = osc.encode_message(full, args)
                    self._socket().sendto(packet, (host, port))
                except (OSError, osc.OscError) as exc:
                    self.failed += 1
                    self.last_error = str(exc)
                    continue
                self.sent += 1
                sent += 1
        return sent

    def stats(self) -> dict:
        with self._lock:
            return {
                "enabled": self._enabled,
                "host": self._host,
                "port": self._port,
                "prefix": self._prefix,
                "sent": self.sent,
                "failed": self.failed,
                "lastError": self.last_error,
            }

    def close(self) -> None:
        with self._lock:
            if self._sock is not None:
                try:
                    self._sock.close()
                except OSError:
                    pass
                self._sock = None
