"""``viewer.conf.json`` -- the viewer's persisted state.

One file holds everything needed to bring the viewer back exactly as it was
left: the OSC endpoint, the HTTP port, which widgets are open, how wide they
are, and every widget's own parameters.

The browser owns the ``ui`` subtree and pushes it back whenever it changes;
the backend owns ``osc`` and ``server``. Nothing validates the ``ui`` blob --
it is the client's own state, and a backend that second-guessed its shape
would have to be edited every time a widget grew a checkbox.
"""

from __future__ import annotations

import copy
import json
import os
import tempfile
import threading
from typing import Any, Dict

# Defaults are also the schema: anything the UI may read must exist here, so
# that a first run and a run against a hand-trimmed config behave the same.
DEFAULTS: Dict[str, Any] = {
    "osc": {
        # 0.0.0.0 rather than 127.0.0.1: the DAT may well be running on the
        # machine with the dongle attached rather than on this one.
        "bind": "0.0.0.0",
        "port": 9000,
        "prefix": "/epoc",
    },
    "server": {
        "host": "127.0.0.1",
        "port": 8420,
        "open_browser": True,
    },
    # Where analysis tools will send their own OSC. Unused until the first
    # such tool exists, but the endpoint is configurable from day one so the
    # config file does not have to change shape later.
    "output": {
        "enabled": False,
        "host": "127.0.0.1",
        "port": 9100,
        "prefix": "/xavier",
    },
    "ui": {
        "paused": False,
        "barsCollapsed": False,
        # Widget order is the order of this list; `open` and `width` (a
        # flex weight) are restored on load.
        "layout": [
            {"id": "data", "open": True, "width": 2},
            {"id": "gyro", "open": False, "width": 1},
        ],
        "data": {
            "channels": [True] * 14,
            "units": "uv",            # "uv" | "counts"
            "showRaw": True,
            "showFft": True,
            "windowSeconds": 8,
            "scale": "auto",          # "auto" | a number, in display units
            "removeDc": True,
            "fftScale": "log",        # "log" | "linear"
            "fftShared": True,        # one FFT range for all channels
            "fftMaxHz": 45,
            "showQuality": True,
        },
        "gyro": {
            "showRate": True,
            "showAngle": True,
            "invertX": False,         # which way is "up" depends on the head
            "invertY": False,
            "highpassHz": 0.05,
            "gain": 1.0,
            "windowSeconds": 12,
            "bubbleRange": 60,
        },
    },
}


def _merge(base: Dict[str, Any], patch: Dict[str, Any]) -> Dict[str, Any]:
    """Recursively merge *patch* into a copy of *base*.

    Dicts merge key by key; everything else -- lists included -- replaces
    wholesale. Lists are deliberately not merged elementwise: the channel
    toggle array and the widget layout are both lists whose meaning comes
    from their position, and a half-merged one would be nonsense.
    """
    out = dict(base)
    for key, value in patch.items():
        if isinstance(value, dict) and isinstance(out.get(key), dict):
            out[key] = _merge(out[key], value)
        else:
            out[key] = copy.deepcopy(value)
    return out


class Config:
    """The config file, loaded once and written back on every change.

    Thread-safe: the HTTP threads write it and the startup path reads it.
    Writes are atomic (temp file plus replace), so a viewer killed mid-save
    cannot leave a truncated config behind -- which would otherwise be a
    particularly annoying way to lose a carefully arranged layout.
    """

    def __init__(self, path: str) -> None:
        self.path = os.path.abspath(path)
        self._lock = threading.Lock()
        self._data = copy.deepcopy(DEFAULTS)
        self.existed = False
        self.load()

    def load(self) -> None:
        try:
            with open(self.path, "r", encoding="utf-8") as handle:
                stored = json.load(handle)
        except FileNotFoundError:
            # Write the defaults out immediately rather than waiting for the
            # first change. A file that exists can be read and edited; one
            # that appears only on a clean shutdown cannot.
            self._write(self._data)
            return
        except (OSError, ValueError) as exc:
            # A corrupt config must not stop the viewer from starting; the
            # defaults are always a working configuration.
            print("xavier-viewer: ignoring unreadable {}: {}".format(self.path, exc))
            return
        self.existed = True
        if isinstance(stored, dict):
            with self._lock:
                self._data = _merge(DEFAULTS, stored)

    def snapshot(self) -> Dict[str, Any]:
        with self._lock:
            return copy.deepcopy(self._data)

    def get(self, section: str) -> Dict[str, Any]:
        with self._lock:
            return copy.deepcopy(self._data.get(section, {}))

    def update(self, patch: Dict[str, Any]) -> Dict[str, Any]:
        """Merge *patch* in, persist, and return the new whole."""
        with self._lock:
            self._data = _merge(self._data, patch)
            data = copy.deepcopy(self._data)
        self._write(data)
        return data

    def _write(self, data: Dict[str, Any]) -> None:
        directory = os.path.dirname(self.path) or "."
        try:
            os.makedirs(directory, exist_ok=True)
            handle = tempfile.NamedTemporaryFile(
                "w", encoding="utf-8", dir=directory, prefix=".viewer.conf.",
                suffix=".tmp", delete=False,
            )
            try:
                json.dump(data, handle, indent=2, sort_keys=False)
                handle.write("\n")
                handle.flush()
                os.fsync(handle.fileno())
            finally:
                handle.close()
            os.replace(handle.name, self.path)
        except OSError as exc:
            print("xavier-viewer: could not save {}: {}".format(self.path, exc))

    def save(self) -> None:
        self._write(self.snapshot())
