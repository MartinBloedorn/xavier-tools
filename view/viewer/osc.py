"""Minimal OSC 1.0 codec -- the receive half of ``epoc/src/osc.cpp``.

The C++ side hand-rolls its encoder and deliberately implements "only what
this tool sends". This is the mirror of that: only what this tool needs to
*receive*, plus a small encoder for the OSC the viewer will eventually emit
from its own analysis tools.

The two rules that govern the byte layout are the same ones documented in
``epoc/doc/osc.md``, and are the two that are easy to get wrong:

* strings are NUL-terminated and padded to a multiple of four, with *always
  at least one* terminator -- so a 4-character address occupies 8 bytes;
* all numeric arguments are big-endian, regardless of host byte order.

Bundles are decoded even though the DAT never sends them. It is fifteen
lines, and it means a receiver sitting behind some other OSC router still
works.
"""

from __future__ import annotations

import struct
from typing import Any, Iterator, List, Sequence, Tuple

# NTP counts from 1900-01-01; UNIX from 1970-01-01. The offset is the 70
# years between them, including 17 leap days. Same constant as
# ``unix_ms_to_timetag()`` in osc.cpp.
NTP_UNIX_OFFSET = 2208988800
_TWO32 = 4294967296.0


class OscError(ValueError):
    """A packet that is not decodable as OSC."""


class TimeTag:
    """An OSC time tag (``t``): 64-bit NTP, seconds since 1900 in the high
    word and a 2^-32 second fraction in the low word.

    Kept as a distinct type rather than collapsed to a float so the
    dispatcher can recognise a timestamp argument by its *type* instead of
    by its position -- the DAT can be told to send the timestamp as ``h``,
    ``d``, ``i`` or ``t``, and only ``t`` is self-describing.
    """

    __slots__ = ("ntp",)

    def __init__(self, ntp: int) -> None:
        self.ntp = ntp

    @property
    def unix(self) -> float:
        """Seconds since the UNIX epoch, as a float."""
        if self.ntp == 0:  # the "immediately" time tag
            return 0.0
        seconds = self.ntp >> 32
        fraction = self.ntp & 0xFFFFFFFF
        return (seconds - NTP_UNIX_OFFSET) + fraction / _TWO32

    def __repr__(self) -> str:  # pragma: no cover - debugging aid
        return "TimeTag({:.6f})".format(self.unix)


# --------------------------------------------------------------------------
# Decoding
# --------------------------------------------------------------------------

def _read_string(data: bytes, pos: int) -> Tuple[str, int]:
    """Read a padded, NUL-terminated OSC string starting at *pos*."""
    end = data.find(b"\x00", pos)
    if end < 0:
        raise OscError("unterminated string")
    text = data[pos:end].decode("utf-8", "replace")
    # Always at least one terminator, then pad to a multiple of four.
    return text, pos + (end - pos) // 4 * 4 + 4


def _read_blob(data: bytes, pos: int) -> Tuple[bytes, int]:
    if pos + 4 > len(data):
        raise OscError("truncated blob length")
    (size,) = struct.unpack_from(">i", data, pos)
    pos += 4
    if size < 0 or pos + size > len(data):
        raise OscError("truncated blob")
    payload = data[pos:pos + size]
    return payload, pos + (size + 3) // 4 * 4


# Fixed-width scalar types: tag -> (struct format, byte width).
_SCALARS = {
    "i": (">i", 4),
    "f": (">f", 4),
    "h": (">q", 8),
    "d": (">d", 8),
    "t": (">Q", 8),
    "c": (">I", 4),
    "r": (">I", 4),
    "m": (">I", 4),
}

# Types that carry no bytes at all; the tag *is* the value.
_EMPTY = {"T": True, "F": False, "N": None, "I": float("inf")}


def _read_args(data: bytes, pos: int, tags: str) -> List[Any]:
    args: List[Any] = []
    for tag in tags:
        if tag in _EMPTY:
            args.append(_EMPTY[tag])
            continue
        if tag in ("s", "S"):
            value, pos = _read_string(data, pos)
            args.append(value)
            continue
        if tag == "b":
            blob, pos = _read_blob(data, pos)
            args.append(blob)
            continue
        spec = _SCALARS.get(tag)
        if spec is None:
            # An unknown tag has an unknown width, so the rest of the
            # argument list is no longer parseable. Keep what we have.
            raise OscError("unsupported type tag {!r}".format(tag))
        fmt, width = spec
        if pos + width > len(data):
            raise OscError("truncated argument for tag {!r}".format(tag))
        (value,) = struct.unpack_from(fmt, data, pos)
        pos += width
        args.append(TimeTag(value) if tag == "t" else value)
    return args


def _decode_message(data: bytes) -> Tuple[str, List[Any]]:
    address, pos = _read_string(data, 0)
    if pos >= len(data):
        return address, []  # no type-tag string: legal, if unusual
    tags, pos = _read_string(data, pos)
    if not tags.startswith(","):
        raise OscError("type tag string does not start with ','")
    return address, _read_args(data, pos, tags[1:])


def decode_packet(data: bytes) -> List[Tuple[str, List[Any]]]:
    """Decode one datagram into a list of ``(address, args)`` pairs.

    A plain message yields one pair; a bundle yields one per contained
    element, recursively. Raises :class:`OscError` on anything malformed --
    callers on the UDP path should treat that as "not our traffic" rather
    than as a fault, since any host on the network can send us bytes.
    """
    if len(data) % 4 != 0 or not data:
        raise OscError("packet length {} is not a positive multiple of 4".format(len(data)))
    if data.startswith(b"#bundle\x00"):
        return list(_decode_bundle(data))
    if not data.startswith(b"/"):
        raise OscError("not an address pattern or bundle")
    return [_decode_message(data)]


def _decode_bundle(data: bytes) -> Iterator[Tuple[str, List[Any]]]:
    pos = 16  # "#bundle\0" plus the bundle's own time tag
    while pos + 4 <= len(data):
        (size,) = struct.unpack_from(">i", data, pos)
        pos += 4
        if size < 0 or pos + size > len(data):
            raise OscError("truncated bundle element")
        element = data[pos:pos + size]
        pos += size
        if element.startswith(b"#bundle\x00"):
            for item in _decode_bundle(element):
                yield item
        elif element:
            yield _decode_message(element)


# --------------------------------------------------------------------------
# Encoding
# --------------------------------------------------------------------------

def _pad(raw: bytes) -> bytes:
    """NUL-terminate and pad to a multiple of four, always at least one NUL."""
    return raw + b"\x00" * (4 - len(raw) % 4)


def encode_message(address: str, args: Sequence[Any] = ()) -> bytes:
    """Encode one OSC message.

    Python ints map to ``i`` when they fit in 32 bits and ``h`` otherwise,
    floats to ``f``, ``str`` to ``s``, ``bytes`` to ``b`` and
    :class:`TimeTag` to ``t``. This exists for the analysis-tool output
    path; nothing on the receive side uses it.
    """
    tags = ","
    payload = bytearray()
    for arg in args:
        if isinstance(arg, TimeTag):
            tags += "t"
            payload += struct.pack(">Q", arg.ntp)
        elif isinstance(arg, bool):
            tags += "T" if arg else "F"
        elif isinstance(arg, int):
            if -2147483648 <= arg <= 2147483647:
                tags += "i"
                payload += struct.pack(">i", arg)
            else:
                tags += "h"
                payload += struct.pack(">q", arg)
        elif isinstance(arg, float):
            tags += "f"
            payload += struct.pack(">f", arg)
        elif isinstance(arg, str):
            tags += "s"
            payload += _pad(arg.encode("utf-8"))
        elif isinstance(arg, (bytes, bytearray)):
            # Blobs pad to a multiple of four but, unlike strings, take no
            # terminator -- so a 4-byte blob really is 4 bytes.
            tags += "b"
            payload += struct.pack(">i", len(arg)) + bytes(arg)
            payload += b"\x00" * (-len(arg) % 4)
        else:
            raise OscError("cannot encode argument of type {}".format(type(arg).__name__))
    return _pad(address.encode("utf-8")) + _pad(tags.encode("utf-8")) + bytes(payload)


def unix_to_timetag(unix_seconds: float) -> TimeTag:
    """Inverse of :attr:`TimeTag.unix`."""
    if unix_seconds <= 0:
        return TimeTag(0)
    total = unix_seconds + NTP_UNIX_OFFSET
    seconds = int(total)
    fraction = int((total - seconds) * _TWO32) & 0xFFFFFFFF
    return TimeTag((seconds << 32) | fraction)


def normalize_prefix(prefix: str) -> str:
    """Exactly one leading ``/``, no trailing ``/``, no repeated slashes.

    Mirrors ``normalize_prefix()`` in osc.cpp so that whatever the user types
    into the DAT and whatever they type into the viewer compare equal.
    """
    parts = [p for p in prefix.split("/") if p]
    return "/" + "/".join(parts) if parts else "/"
