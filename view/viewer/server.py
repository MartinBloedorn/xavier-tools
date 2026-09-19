"""HTTP: static assets, the live data stream, and the control API.

Standard library only, matching the rest of the project's posture on
dependencies -- ``./xavier-viewer`` runs against a stock Python with nothing
to install first.

The live stream is **server-sent events** rather than a WebSocket. The data
only ever flows one way, SSE needs no framing layer, and the browser
reconnects on its own when the server restarts. Control goes the other way
as ordinary JSON POSTs, which is also how the analysis tools will eventually
ask for OSC to be emitted.
"""

from __future__ import annotations

import json
import mimetypes
import os
import posixpath
import socket
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any, Callable, Dict, Optional

from .state import (
    BANDS,
    CHANNELS,
    Hub,
    MICROVOLTS_PER_COUNT,
    NOMINAL_SAMPLE_RATE,
    QUALITY_FULL_SCALE,
)

WEB_ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "web")

# How often a connected browser is served a batch. At 128 Hz this bundles
# about three samples per event, which is below one frame at 60 Hz -- the
# display cannot show more, and smaller batches would only add overhead.
TICK_SECONDS = 1.0 / 40.0

# Even with no data at all, say something this often: it keeps the
# connection indicator honest, proves the socket is alive, and stops
# intermediaries from timing out an idle response.
HEARTBEAT_SECONDS = 0.25

mimetypes.add_type("text/javascript", ".js")
mimetypes.add_type("text/css", ".css")


class ViewerServer:
    """Owns the HTTP server and the objects the handler needs."""

    def __init__(self, hub: Hub, config, receiver, sender,
                 host: str = "127.0.0.1", port: int = 8420,
                 verbose: bool = False) -> None:
        self.hub = hub
        self.config = config
        self.receiver = receiver
        self.sender = sender
        self.verbose = verbose
        self.started = time.time()

        handler = _make_handler(self)
        self._httpd = ThreadingHTTPServer((host, port), handler)
        self._httpd.daemon_threads = True
        self.host, self.port = self._httpd.server_address[:2]
        self._thread: Optional[threading.Thread] = None

    @property
    def url(self) -> str:
        shown = "127.0.0.1" if self.host in ("0.0.0.0", "") else self.host
        return "http://{}:{}/".format(shown, self.port)

    def start(self) -> None:
        self._thread = threading.Thread(
            target=self._httpd.serve_forever, kwargs={"poll_interval": 0.2},
            name="http", daemon=True,
        )
        self._thread.start()

    def stop(self) -> None:
        self._httpd.shutdown()
        self._httpd.server_close()
        if self._thread is not None:
            self._thread.join(timeout=2.0)

    # -- payloads shared by several routes ---------------------------------

    def describe(self) -> Dict[str, Any]:
        """Everything the browser needs that never changes mid-session,
        plus the current configuration."""
        bind, port, prefix = self.receiver.endpoint
        return {
            "channels": list(CHANNELS),
            "bands": [{"name": n, "lo": lo, "hi": hi} for n, lo, hi in BANDS],
            "uvPerCount": MICROVOLTS_PER_COUNT,
            "sampleRate": NOMINAL_SAMPLE_RATE,
            "qualityFullScale": QUALITY_FULL_SCALE,
            "config": self.config.snapshot(),
            "osc": {"bind": bind, "port": port, "prefix": prefix,
                    "error": self.receiver.last_error},
            "output": self.sender.stats(),
            "t0": self.hub.t0,
        }


def _make_handler(app: ViewerServer):
    """Build a handler class bound to *app*.

    A closure rather than an attribute on the server object, so the handler
    cannot be constructed without its dependencies.
    """

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"
        server_version = "xavier-viewer"
        sys_version = ""

        # -- plumbing ---------------------------------------------------

        def log_message(self, fmt: str, *args: Any) -> None:
            if app.verbose:
                BaseHTTPRequestHandler.log_message(self, fmt, *args)

        def log_error(self, fmt: str, *args: Any) -> None:
            # A browser closing a tab aborts the SSE response, which the
            # base class reports as an error. It is not one.
            if app.verbose:
                BaseHTTPRequestHandler.log_error(self, fmt, *args)

        def _send(self, status: int, body: bytes, content_type: str,
                  extra: Optional[Dict[str, str]] = None) -> None:
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            # The viewer is a local tool that people will edit and reload;
            # a cached stale app.js is a worse failure than a re-fetch.
            self.send_header("Cache-Control", "no-store")
            for key, value in (extra or {}).items():
                self.send_header(key, value)
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(body)

        def _json(self, payload: Any, status: int = 200) -> None:
            self._send(status, json.dumps(payload).encode("utf-8"),
                       "application/json; charset=utf-8")

        def _error(self, status: int, message: str) -> None:
            self._json({"error": message}, status)

        def _read_json(self) -> Optional[Dict[str, Any]]:
            try:
                length = int(self.headers.get("Content-Length") or 0)
            except ValueError:
                return None
            if length <= 0 or length > 4 << 20:
                return None
            try:
                body = self.rfile.read(length)
                parsed = json.loads(body.decode("utf-8"))
            except (OSError, ValueError):
                return None
            return parsed if isinstance(parsed, dict) else None

        # -- routes -----------------------------------------------------

        def do_GET(self) -> None:  # noqa: N802 - name fixed by the base class
            path = self.path.split("?", 1)[0]
            if path == "/api/stream":
                self._stream()
            elif path == "/api/config":
                self._json(app.describe())
            elif path == "/api/status":
                self._json(app.hub.status())
            elif path.startswith("/api/"):
                self._error(404, "no such endpoint")
            else:
                self._static(path)

        def do_HEAD(self) -> None:  # noqa: N802
            path = self.path.split("?", 1)[0]
            if path.startswith("/api/"):
                self._error(405, "HEAD not supported on the API")
            else:
                self._static(path)

        def do_POST(self) -> None:  # noqa: N802
            path = self.path.split("?", 1)[0]
            handlers: Dict[str, Callable[[Dict[str, Any]], None]] = {
                "/api/config": self._post_config,
                "/api/osc": self._post_osc,
                "/api/output": self._post_output,
                "/api/emit": self._post_emit,
            }
            handler = handlers.get(path)
            if handler is None:
                self._error(404, "no such endpoint")
                return
            payload = self._read_json()
            if payload is None:
                self._error(400, "expected a JSON object body")
                return
            handler(payload)

        # -- static -----------------------------------------------------

        def _static(self, path: str) -> None:
            if path in ("/", ""):
                path = "/index.html"
            # Normalise before joining: posixpath.normpath collapses the
            # ".." that would otherwise walk out of the web root.
            relative = posixpath.normpath(path).lstrip("/")
            target = os.path.normpath(os.path.join(WEB_ROOT, relative))
            try:
                inside = os.path.commonpath([target, WEB_ROOT]) == WEB_ROOT
            except ValueError:
                # Different drives on Windows -- a request for "/C:/..."
                # joins to an absolute path that escapes the root entirely.
                inside = False
            if not inside:
                self._error(403, "outside the web root")
                return
            try:
                with open(target, "rb") as handle:
                    body = handle.read()
            except (OSError, ValueError):
                self._error(404, "not found")
                return
            content_type = mimetypes.guess_type(target)[0] or "application/octet-stream"
            if content_type.startswith(("text/", "application/json", "image/svg")):
                content_type += "; charset=utf-8"
            self._send(200, body, content_type)

        # -- the live stream --------------------------------------------

        def _stream(self) -> None:
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream; charset=utf-8")
            self.send_header("Cache-Control", "no-cache, no-store, must-revalidate")
            # No Content-Length and no chunking: the body is delimited by
            # the close, which is what SSE wants and what every browser
            # implements. Say so explicitly so keep-alive is not attempted.
            self.send_header("Connection", "close")
            # Nginx and friends buffer text/* by default, which would turn a
            # live stream into a stuttering one. Harmless when direct.
            self.send_header("X-Accel-Buffering", "no")
            self.end_headers()
            self.close_connection = True

            try:
                # The retry field is the browser's reconnect delay. Short,
                # because the usual reason for a drop is the viewer being
                # restarted during development.
                self._write_raw("retry: 750\n\n")
                self._write_event(app.describe(), event="init")
            except OSError:
                return

            sub = app.hub.subscribe()
            last_send = 0.0
            next_tick = time.time()
            try:
                while True:
                    now = time.time()
                    if now < next_tick:
                        time.sleep(next_tick - now)
                        continue
                    # Pace off the clock, not off arrivals: waking per
                    # sample would send 128 single-frame events a second.
                    # Re-base rather than accumulate, so a descheduled
                    # thread catches up in one tick instead of bursting.
                    next_tick = max(now, next_tick + TICK_SECONDS)
                    payload = app.hub.drain(sub, now)
                    has_data = len(payload) > 1  # more than just "status"
                    if not has_data and now - last_send < HEARTBEAT_SECONDS:
                        continue
                    self._write_event(payload)
                    last_send = now
            except (OSError, ValueError):
                pass  # the browser went away; entirely routine
            finally:
                app.hub.unsubscribe(sub)

        def _write_raw(self, text: str) -> None:
            self.wfile.write(text.encode("utf-8"))
            self.wfile.flush()

        def _write_event(self, payload: Any, event: Optional[str] = None) -> None:
            head = "event: {}\n".format(event) if event else ""
            self._write_raw("{}data: {}\n\n".format(head, json.dumps(payload)))

        # -- control ----------------------------------------------------

        def _post_config(self, payload: Dict[str, Any]) -> None:
            """Persist a config patch.

            The browser owns the ``ui`` subtree and posts it here whenever
            something changes; ``osc`` and ``output`` also arrive this way
            when their forms are submitted, and are applied, not just saved.
            """
            patch = {k: v for k, v in payload.items()
                     if k in ("ui", "osc", "server", "output") and isinstance(v, dict)}
            if not patch:
                self._error(400, "nothing to update")
                return
            merged = app.config.update(patch)
            if "osc" in patch:
                osc_cfg = merged["osc"]
                app.receiver.repoint(osc_cfg.get("bind"), osc_cfg.get("port"),
                                     osc_cfg.get("prefix"))
            if "output" in patch:
                out = merged["output"]
                app.sender.configure(out.get("host"), out.get("port"),
                                     out.get("prefix"), out.get("enabled"))
            self._json(app.describe())

        def _post_osc(self, payload: Dict[str, Any]) -> None:
            """Re-point the OSC input. Also persisted, so a corrected port
            survives a restart -- which is the whole reason for editing it
            here rather than on the command line."""
            patch: Dict[str, Any] = {}
            for key in ("bind", "prefix"):
                if isinstance(payload.get(key), str):
                    patch[key] = payload[key]
            port = payload.get("port")
            if port is not None:
                try:
                    port = int(port)
                except (TypeError, ValueError):
                    self._error(400, "port must be a number")
                    return
                if not 1 <= port <= 65535:
                    self._error(400, "port must be in 1..65535")
                    return
                patch["port"] = port
            if not patch:
                self._error(400, "nothing to change")
                return
            app.config.update({"osc": patch})
            app.receiver.repoint(patch.get("bind"), patch.get("port"),
                                 patch.get("prefix"))
            self._json(app.describe())

        def _post_output(self, payload: Dict[str, Any]) -> None:
            app.sender.configure(
                payload.get("host"), payload.get("port"),
                payload.get("prefix"), payload.get("enabled"),
            )
            stats = app.sender.stats()
            app.config.update({"output": {
                "enabled": stats["enabled"], "host": stats["host"],
                "port": stats["port"], "prefix": stats["prefix"],
            }})
            self._json(app.describe())

        def _post_emit(self, payload: Dict[str, Any]) -> None:
            """Emit one OSC message on behalf of an in-viewer analysis tool.

            Nothing uses this yet. It is the seam the planning note asks for,
            and having it exercised from the start means the first tool to
            need it finds a working path rather than an untested one.
            """
            address = payload.get("address")
            if not isinstance(address, str) or not address:
                self._error(400, "address must be a non-empty string")
                return
            args = payload.get("args", [])
            if not isinstance(args, list):
                self._error(400, "args must be a list")
                return
            ok = app.sender.send(address, args)
            self._json({"sent": ok, "output": app.sender.stats()})

    return Handler


def find_free_port(host: str, preferred: int) -> int:
    """Return *preferred* if it can be bound, otherwise an ephemeral port.

    Losing the saved HTTP port to some unrelated process should not stop the
    viewer from starting -- the URL is printed on the console either way.
    """
    probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        probe.bind((host, preferred))
        return preferred
    except OSError:
        pass
    finally:
        probe.close()
    probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        probe.bind((host, 0))
        return int(probe.getsockname()[1])
    finally:
        probe.close()
