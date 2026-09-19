"""Wiring and command line for ``xavier-viewer``.

Order matters here for the same reason it does in the C++ tool's ``run()``:
everything parseable is settled before anything is bound, so a typo fails
immediately instead of after a socket is already listening.
"""

from __future__ import annotations

import argparse
import os
import signal
import sys
import threading
import webbrowser
from typing import List, Optional

from . import osc
from .config import Config
from .receiver import Receiver
from .sender import Sender
from .server import ViewerServer, find_free_port
from .state import Hub

VERSION = "0.1.0"

# Next to the entry-point script rather than in the working directory: the
# viewer is started from wherever, and a layout that moved with the shell's
# cwd would be a surprising thing to lose.
DEFAULT_CONFIG = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "viewer.conf.json"
)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="xavier-viewer",
        description="Web viewer for the OSC stream produced by the epoc CLI.",
        epilog=(
            "Start the acquisition tool with the message set this viewer "
            "expects:\n"
            "  epoc --osc 9000 --osc-messages tbugyq --osc-timestamp timetag\n"
            "\n"
            "Command-line values override the saved configuration and are "
            "written back to it."
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--osc-port", type=int, metavar="N",
                        help="UDP port to receive OSC on (default: 9000)")
    parser.add_argument("--osc-bind", metavar="ADDR",
                        help="address to bind the OSC socket to (default: 0.0.0.0)")
    parser.add_argument("--osc-prefix", metavar="P",
                        help="OSC address prefix the DAT is using (default: /epoc)")
    parser.add_argument("--port", type=int, metavar="N",
                        help="HTTP port for the web view (default: 8420)")
    parser.add_argument("--host", metavar="ADDR",
                        help="address to serve the web view on (default: 127.0.0.1)")
    parser.add_argument("--config", metavar="FILE", default=DEFAULT_CONFIG,
                        help="configuration file (default: view/viewer.conf.json)")
    parser.add_argument("--no-browser", action="store_true",
                        help="do not open a browser on startup")
    parser.add_argument("--browser", action="store_true",
                        help="open a browser on startup even if the config says not to")
    parser.add_argument("--reset", action="store_true",
                        help="ignore the saved configuration and start from defaults")
    parser.add_argument("--verbose", action="store_true",
                        help="log HTTP requests")
    parser.add_argument("--version", action="version",
                        version="xavier-viewer {}".format(VERSION))
    return parser


def main(argv: Optional[List[str]] = None) -> int:
    args = build_parser().parse_args(argv)

    if args.reset:
        try:
            os.remove(args.config)
        except OSError:
            pass

    config = Config(args.config)

    # Command-line overrides are persisted, so `--osc-port 9100` once is
    # equivalent to editing the config: the next bare run keeps it.
    overrides = {}
    if args.osc_port is not None:
        if not 1 <= args.osc_port <= 65535:
            print("xavier-viewer: --osc-port must be in 1..65535", file=sys.stderr)
            return 2
        overrides.setdefault("osc", {})["port"] = args.osc_port
    if args.osc_bind is not None:
        overrides.setdefault("osc", {})["bind"] = args.osc_bind
    if args.osc_prefix is not None:
        overrides.setdefault("osc", {})["prefix"] = osc.normalize_prefix(args.osc_prefix)
    if args.port is not None:
        if not 1 <= args.port <= 65535:
            print("xavier-viewer: --port must be in 1..65535", file=sys.stderr)
            return 2
        overrides.setdefault("server", {})["port"] = args.port
    if args.host is not None:
        overrides.setdefault("server", {})["host"] = args.host
    if overrides:
        config.update(overrides)

    osc_cfg = config.get("osc")
    server_cfg = config.get("server")
    output_cfg = config.get("output")

    hub = Hub()
    receiver = Receiver(hub, osc_cfg["bind"], osc_cfg["port"], osc_cfg["prefix"])
    sender = Sender(output_cfg["host"], output_cfg["port"],
                    output_cfg["prefix"], output_cfg["enabled"])

    host = server_cfg["host"]
    wanted = int(server_cfg["port"])
    port = find_free_port(host, wanted)
    if port != wanted:
        print("xavier-viewer: port {} is busy, using {} instead".format(wanted, port))

    try:
        server = ViewerServer(hub, config, receiver, sender,
                              host=host, port=port, verbose=args.verbose)
    except OSError as exc:
        print("xavier-viewer: cannot serve on {}:{} -- {}".format(host, port, exc),
              file=sys.stderr)
        return 1

    receiver.start()
    server.start()

    bind, osc_port, prefix = receiver.endpoint
    print("xavier-viewer {} -- web view at {}".format(VERSION, server.url))
    print("xavier-viewer: expecting OSC on {}:{}, prefix {}"
          .format(bind, osc_port, prefix))
    print("xavier-viewer: config {}{}"
          .format(config.path, "" if config.existed else " (created)"))
    print("xavier-viewer: start the acquisition tool with "
          "--osc-messages tbugyq --osc-timestamp timetag")
    # The banner is the only output until something goes wrong, and stdout
    # is block-buffered when redirected -- without this it appears only at
    # exit, which makes a redirected run look like it hung.
    sys.stdout.flush()

    open_browser = server_cfg.get("open_browser", True)
    if args.browser:
        open_browser = True
    if args.no_browser:
        open_browser = False
    if open_browser:
        # Deferred: the server is already listening, but Chrome occasionally
        # races a connection made in the same millisecond as the listen.
        threading.Timer(0.4, _open, args=(server.url,)).start()

    stop = threading.Event()

    def _shutdown(signum, frame):  # noqa: ANN001 - signal handler signature
        stop.set()

    signal.signal(signal.SIGINT, _shutdown)
    try:
        signal.signal(signal.SIGTERM, _shutdown)
    except (AttributeError, ValueError):
        pass  # not available on every platform

    try:
        while not stop.is_set():
            # A plain wait() is not interruptible by Ctrl-C on Windows;
            # a short poll is.
            stop.wait(0.25)
    except KeyboardInterrupt:
        pass

    print("\nxavier-viewer: shutting down")
    receiver.stop()
    server.stop()
    sender.close()
    config.save()

    status = hub.status()
    print("xavier-viewer: {} packets, {} samples, {} malformed, {} not matching prefix"
          .format(status["packets"], status["samples"], status["malformed"],
                  status["unmatched"]))
    return 0


def _open(url: str) -> None:
    try:
        webbrowser.open(url)
    except Exception:  # pragma: no cover - platform dependent
        pass
