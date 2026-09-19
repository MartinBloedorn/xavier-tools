#!/usr/bin/env python3
"""Drive a headless Chrome over the DevTools protocol, with no dependencies.

Why this exists: the viewer holds an SSE connection open for as long as the
page is up, so the page never reaches "load finished" and
``chrome --screenshot`` waits forever. Driving the browser directly also
gets at the thing that actually matters when working on the UI -- console
errors -- which a screenshot cannot show.

    python tools/cdp.py http://127.0.0.1:8420/ --wait 4 --shot out.png
    python tools/cdp.py http://127.0.0.1:8420/ --eval "app.panels.size"

Speaks just enough RFC 6455 to carry JSON both ways: a client-masked text
frame out, an unmasked frame in, ping answered with pong. No extensions, no
permessage-deflate.
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.request
from typing import Any, Dict, List, Optional

CHROME_CANDIDATES = [
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
    os.path.expandvars(r"%LOCALAPPDATA%\Google\Chrome\Application\chrome.exe"),
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
    "/usr/bin/google-chrome",
    "/usr/bin/chromium",
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
]


def find_chrome() -> Optional[str]:
    for candidate in CHROME_CANDIDATES:
        if candidate and os.path.exists(candidate):
            return candidate
    for name in ("google-chrome", "chromium", "chrome"):
        found = shutil.which(name)
        if found:
            return found
    return None


# --------------------------------------------------------------- websocket

class WebSocket:
    """A minimal client. Text frames only, which is all CDP uses."""

    def __init__(self, url: str, timeout: float = 20.0) -> None:
        if not url.startswith("ws://"):
            raise ValueError("only ws:// is supported")
        rest = url[5:]
        hostport, _, path = rest.partition("/")
        host, _, port = hostport.partition(":")
        self.sock = socket.create_connection((host, int(port or 80)), timeout=timeout)
        self.sock.settimeout(timeout)
        self.buffer = b""

        key = base64.b64encode(os.urandom(16)).decode()
        request = (
            "GET /{} HTTP/1.1\r\n"
            "Host: {}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: {}\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n"
        ).format(path, hostport, key)
        self.sock.sendall(request.encode())

        while b"\r\n\r\n" not in self.buffer:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("handshake closed early")
            self.buffer += chunk
        head, _, self.buffer = self.buffer.partition(b"\r\n\r\n")
        if b"101" not in head.split(b"\r\n")[0]:
            raise ConnectionError("upgrade refused: " + head.split(b"\r\n")[0].decode())

    def _read(self, count: int) -> bytes:
        while len(self.buffer) < count:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("socket closed")
            self.buffer += chunk
        out, self.buffer = self.buffer[:count], self.buffer[count:]
        return out

    def send(self, text: str) -> None:
        payload = text.encode("utf-8")
        header = bytearray([0x81])  # FIN + text
        size = len(payload)
        if size < 126:
            header.append(0x80 | size)
        elif size < 65536:
            header.append(0x80 | 126)
            header += struct.pack(">H", size)
        else:
            header.append(0x80 | 127)
            header += struct.pack(">Q", size)
        mask = os.urandom(4)
        header += mask
        masked = bytearray(payload)
        for i in range(size):
            masked[i] ^= mask[i & 3]
        self.sock.sendall(bytes(header) + bytes(masked))

    def recv(self) -> str:
        """Next complete text message, reassembling continuation frames."""
        chunks: List[bytes] = []
        while True:
            first, second = self._read(2)
            fin = first & 0x80
            opcode = first & 0x0F
            length = second & 0x7F
            if length == 126:
                (length,) = struct.unpack(">H", self._read(2))
            elif length == 127:
                (length,) = struct.unpack(">Q", self._read(8))
            payload = self._read(length) if length else b""
            if second & 0x80:  # a server frame should never be masked
                raise ConnectionError("masked frame from server")

            if opcode == 0x9:                      # ping -> pong
                self.sock.sendall(b"\x8a\x80" + os.urandom(4))
                continue
            if opcode == 0xA:                      # pong
                continue
            if opcode == 0x8:
                raise ConnectionError("closed by peer")
            chunks.append(payload)
            if fin:
                return b"".join(chunks).decode("utf-8", "replace")

    def close(self) -> None:
        try:
            self.sock.sendall(b"\x88\x80" + os.urandom(4))
        except OSError:
            pass
        self.sock.close()


# --------------------------------------------------------------------- CDP

class Chrome:
    def __init__(self, url: str, port: int = 9222, width: int = 1600,
                 height: int = 900, headless: bool = True) -> None:
        binary = find_chrome()
        if not binary:
            raise SystemExit("cdp: no Chrome or Edge found")
        self.profile = tempfile.mkdtemp(prefix="cdp-profile-")
        args = [
            binary,
            "--remote-debugging-port={}".format(port),
            "--user-data-dir={}".format(self.profile),
            "--window-size={},{}".format(width, height),
            "--no-first-run", "--no-default-browser-check",
            "--disable-extensions", "--disable-background-networking",
            "--disable-gpu", "--no-sandbox", "--mute-audio",
            "--hide-scrollbars",
            "about:blank",
        ]
        if headless:
            args.insert(1, "--headless=new")
        self.process = subprocess.Popen(
            args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.port = port
        self.next_id = 0
        self.ws = WebSocket(self._target_ws())
        self.events: List[Dict[str, Any]] = []

    def _target_ws(self, timeout: float = 25.0) -> str:
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            try:
                with urllib.request.urlopen(
                        "http://127.0.0.1:{}/json/list".format(self.port), timeout=2) as response:
                    targets = json.loads(response.read().decode())
                for target in targets:
                    if target.get("type") == "page" and target.get("webSocketDebuggerUrl"):
                        return target["webSocketDebuggerUrl"]
            except Exception as exc:  # noqa: BLE001 - the browser is still starting
                last = exc
            time.sleep(0.25)
        raise SystemExit("cdp: browser did not expose a page target ({})".format(last))

    def call(self, method: str, params: Optional[Dict[str, Any]] = None,
             timeout: float = 30.0) -> Dict[str, Any]:
        self.next_id += 1
        message_id = self.next_id
        self.ws.send(json.dumps({"id": message_id, "method": method,
                                 "params": params or {}}))
        deadline = time.time() + timeout
        while time.time() < deadline:
            message = json.loads(self.ws.recv())
            if message.get("id") == message_id:
                if "error" in message:
                    raise RuntimeError("{}: {}".format(method, message["error"]))
                return message.get("result", {})
            if "method" in message:
                self.events.append(message)
        raise TimeoutError(method)

    def pump(self, seconds: float) -> None:
        """Let the page run, collecting events, for `seconds`."""
        deadline = time.time() + seconds
        self.ws.sock.settimeout(0.3)
        while time.time() < deadline:
            try:
                message = json.loads(self.ws.recv())
            except socket.timeout:
                continue
            if "method" in message:
                self.events.append(message)
        self.ws.sock.settimeout(20.0)

    def console(self) -> List[str]:
        """Console output and uncaught exceptions, as readable lines."""
        out = []
        for event in self.events:
            method = event.get("method")
            params = event.get("params", {})
            if method == "Runtime.consoleAPICalled":
                parts = []
                for arg in params.get("args", []):
                    parts.append(str(arg.get("value", arg.get("description", arg.get("type")))))
                out.append("[{}] {}".format(params.get("type"), " ".join(parts)))
            elif method == "Runtime.exceptionThrown":
                detail = params.get("exceptionDetails", {})
                text = detail.get("exception", {}).get("description") or detail.get("text")
                out.append("[exception] {} (line {})".format(
                    text, detail.get("lineNumber")))
            elif method == "Log.entryAdded":
                entry = params.get("entry", {})
                if entry.get("level") in ("error", "warning"):
                    out.append("[{}] {} {}".format(
                        entry.get("level"), entry.get("text"), entry.get("url") or ""))
        return out

    def evaluate(self, expression: str) -> Any:
        result = self.call("Runtime.evaluate", {
            "expression": expression, "returnByValue": True, "awaitPromise": True,
        })
        if result.get("exceptionDetails"):
            detail = result["exceptionDetails"]
            raise RuntimeError(detail.get("exception", {}).get("description") or detail.get("text"))
        return result.get("result", {}).get("value")

    def screenshot(self, path: str) -> int:
        result = self.call("Page.captureScreenshot", {"format": "png"})
        raw = base64.b64decode(result["data"])
        with open(path, "wb") as handle:
            handle.write(raw)
        return len(raw)

    def close(self) -> None:
        try:
            self.ws.close()
        except Exception:  # noqa: BLE001
            pass
        self.process.terminate()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
        shutil.rmtree(self.profile, ignore_errors=True)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(prog="cdp.py", description=__doc__.split("\n")[0])
    parser.add_argument("url")
    parser.add_argument("--wait", type=float, default=4.0,
                        help="seconds to let the page run before acting")
    parser.add_argument("--shot", metavar="FILE", help="write a PNG screenshot here")
    parser.add_argument("--eval", dest="expressions", action="append", default=[],
                        metavar="JS", help="evaluate an expression and print it (repeatable)")
    parser.add_argument("--eval-file", dest="files", action="append", default=[],
                        metavar="FILE", help="evaluate a file's contents as one expression "
                                             "(repeatable; runs after --eval)")
    parser.add_argument("--after", type=float, default=0.0,
                        help="extra seconds to run after the evaluations")
    parser.add_argument("--width", type=int, default=1600)
    parser.add_argument("--height", type=int, default=900)
    parser.add_argument("--port", type=int, default=9222)
    parser.add_argument("--show", action="store_true", help="run headed")
    args = parser.parse_args(argv)

    browser = Chrome(args.url, port=args.port, width=args.width, height=args.height,
                     headless=not args.show)
    try:
        browser.call("Page.enable")
        browser.call("Runtime.enable")
        browser.call("Log.enable")
        # Not waiting for the load event: the viewer holds an SSE connection
        # open, so "loaded" never arrives.
        browser.call("Page.navigate", {"url": args.url})
        browser.pump(args.wait)

        jobs = [(expression, expression) for expression in args.expressions]
        for path in args.files:
            with open(path, "r", encoding="utf-8") as handle:
                jobs.append((path, handle.read()))
        for name, expression in jobs:
            try:
                print("{} => {}".format(name, json.dumps(browser.evaluate(expression), indent=1)))
            except Exception as exc:  # noqa: BLE001
                print("{} => ERROR {}".format(name, exc))

        if args.after:
            browser.pump(args.after)

        lines = browser.console()
        if lines:
            print("--- console ---")
            for line in lines:
                print(line)
        else:
            print("--- console clean ---")

        if args.shot:
            size = browser.screenshot(args.shot)
            print("--- screenshot {} ({} bytes) ---".format(args.shot, size))
    finally:
        browser.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
