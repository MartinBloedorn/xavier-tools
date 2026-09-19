"""Outbound OSC.

Nothing in the viewer emits OSC yet. The planning note says analysis tools
living in the viewer will eventually want to, so the transport exists now and
the endpoint is already in ``viewer.conf.json`` -- adding the first such tool
should not also mean reshaping the config file and the settings UI.

Mirrors the C++ sender's posture: connectionless UDP, failed sends counted
rather than thrown. A dropped datagram must not take down a live session.
"""

from __future__ import annotations

import socket
import threading
from typing import Any, Optional, Sequence

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
        with self._lock:
            if not self._enabled:
                return False
            prefix, host, port = self._prefix, self._host, self._port
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
                return False
            self.sent += 1
            return True

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
