#!/usr/bin/env python3
"""A synthetic EPOC, for working on the viewer without a headset.

Sends exactly what ``epoc --osc-messages tbugyq --osc-timestamp timetag``
sends, at the same rates and with the same argument types, over real UDP --
so it exercises the viewer's decode path rather than bypassing it.

    python tools/fake_dat.py --osc 9000

What it does *not* do is model the headset: the signal is synthetic EEG
(a DC offset, a per-channel alpha rhythm, pink-ish noise and an occasional
blink artefact) chosen to make the display readable and to put recognisable
structure in every band. Band powers are computed with a real FFT using the
same normalisation as ``epoc/src/dsp.cpp``, so the numbers on screen are
internally consistent with what the C++ tool would produce for this signal.
"""

from __future__ import annotations

import argparse
import cmath
import math
import os
import random
import socket
import sys
import time
from typing import List, Sequence

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from viewer import osc  # noqa: E402
from viewer.state import BANDS, CHANNELS, MICROVOLTS_PER_COUNT  # noqa: E402

SAMPLE_RATE = 128.0
FFT_WINDOW = 128          # epoc defaults to 256; 128 keeps pure Python cheap
FFT_INTERVAL = 1.0 / 16.0  # epoc recomputes per display refresh, ~20 Hz
DC_COUNTS = 8400          # the EPOC's resting offset, about 4.3 mV

# Battery cadence. The dongle substitutes a charge reading for the sequence
# counter once a second, and epoc tests its resend timer only on one of those
# frames -- so BATTERY_RESEND is a floor, not a period, and the gap a receiver
# actually sees is 5-6 s. Modelling the frame rather than the send reproduces
# that slack. BATTERY_DRIFT is ours alone: real charge moves in 13 coarse
# steps and can sit on one for hours, which is exactly why epoc resends.
BATTERY_FRAME = 1.0
BATTERY_RESEND = 5.0      # epoc's kBatteryResend
BATTERY_DRIFT = 60.0


def fft(values: Sequence[complex]) -> List[complex]:
    """Iterative radix-2 Cooley-Tukey, in place on a copy.

    The same algorithm as dsp.cpp; here only so the fake band powers are
    real ones.
    """
    n = len(values)
    out = list(values)
    j = 0
    for i in range(1, n):
        bit = n >> 1
        while j & bit:
            j ^= bit
            bit >>= 1
        j |= bit
        if i < j:
            out[i], out[j] = out[j], out[i]
    length = 2
    while length <= n:
        angle = -2.0 * math.pi / length
        step = cmath.exp(complex(0.0, angle))
        for start in range(0, n, length):
            w = complex(1.0, 0.0)
            half = length >> 1
            for k in range(start, start + half):
                even = out[k]
                odd = out[k + half] * w
                out[k] = even + odd
                out[k + half] = even - odd
                w *= step
        length <<= 1
    return out


class BandAnalyzer:
    """Hann-windowed one-sided PSD integrated per band, in uV^2.

    PSD_k = 2 |X_k|^2 / (fs * S2), S2 = sum(w^2) -- the normalisation that
    makes integrated power independent of window shape and length. See
    epoc/doc/dsp.md.
    """

    def __init__(self, window: int = FFT_WINDOW) -> None:
        self.window = window
        self.hann = [0.5 * (1.0 - math.cos(2.0 * math.pi * i / (window - 1)))
                     for i in range(window)]
        self.s2 = sum(w * w for w in self.hann)
        self.df = SAMPLE_RATE / window

    def power(self, samples: Sequence[float]) -> List[float]:
        n = self.window
        if len(samples) < n:
            return [0.0] * len(BANDS)
        chunk = samples[-n:]
        mean = sum(chunk) / n
        spectrum = fft([complex((chunk[i] - mean) * self.hann[i], 0.0) for i in range(n)])
        scale = 2.0 / (SAMPLE_RATE * self.s2)
        out = [0.0] * len(BANDS)
        for k in range(1, n // 2):  # skip DC and Nyquist, as dsp.cpp does
            freq = k * self.df
            psd = scale * (spectrum[k].real ** 2 + spectrum[k].imag ** 2)
            for b, (_name, lo, hi) in enumerate(BANDS):
                if lo <= freq < hi:
                    out[b] += psd * self.df
                    break
        return out


class Channel:
    """One electrode's synthetic signal."""

    def __init__(self, index: int, rng: random.Random) -> None:
        self.rng = rng
        # Occipital channels get the strongest alpha, which is where it
        # really lives; frontal channels get more of everything else.
        name = CHANNELS[index]
        occipital = name in ("O1", "O2", "P7", "P8")
        frontal = name in ("AF3", "AF4", "F7", "F8", "F3", "F4")
        self.alpha_uv = (14.0 if occipital else 5.0) * rng.uniform(0.7, 1.3)
        self.alpha_hz = rng.uniform(9.4, 10.6)
        self.alpha_phase = rng.uniform(0.0, 2.0 * math.pi)
        self.theta_uv = 6.0 * rng.uniform(0.6, 1.4)
        self.theta_hz = rng.uniform(5.0, 7.0)
        self.beta_uv = (5.0 if frontal else 3.0) * rng.uniform(0.6, 1.4)
        self.beta_hz = rng.uniform(17.0, 23.0)
        self.drift = rng.uniform(-1.0, 1.0)
        self.pink = 0.0
        self.dc = DC_COUNTS + rng.randint(-220, 220)
        self.frontal = frontal
        # Contact quality: a plausible spread, drifting slowly. emokit
        # treats >4000 as good.
        self.quality = rng.uniform(1200, 4600)

    def sample(self, t: float, blink: float) -> int:
        # One-pole lowpass on white noise: a cheap stand-in for the 1/f
        # background that dominates real EEG.
        self.pink = 0.93 * self.pink + 0.07 * self.rng.gauss(0.0, 40.0)
        uv = (
            self.alpha_uv * math.sin(2.0 * math.pi * self.alpha_hz * t + self.alpha_phase)
            + self.theta_uv * math.sin(2.0 * math.pi * self.theta_hz * t)
            + self.beta_uv * math.sin(2.0 * math.pi * self.beta_hz * t)
            + self.pink
            + 2.0 * self.rng.gauss(0.0, 1.0)
            + 8.0 * self.drift * math.sin(2.0 * math.pi * 0.07 * t)
        )
        if self.frontal:
            uv += blink  # blinks are a frontal artefact
        counts = int(round(self.dc + uv / MICROVOLTS_PER_COUNT))
        return max(0, min(16383, counts))  # 14-bit ADC

    def step_quality(self) -> None:
        self.quality = max(0.0, min(5000.0, self.quality + self.rng.gauss(0.0, 30.0)))


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        prog="fake_dat.py",
        description="Send synthetic EPOC OSC traffic to a viewer.",
    )
    parser.add_argument("--osc", default="9000", metavar="DEST",
                        help="host:port or a bare port (default: 9000)")
    parser.add_argument("--prefix", default="/epoc", metavar="P",
                        help="OSC address prefix (default: /epoc)")
    parser.add_argument("--rate", type=float, default=SAMPLE_RATE, metavar="HZ",
                        help="sample rate (default: 128)")
    parser.add_argument("--seed", type=int, default=7,
                        help="RNG seed, so runs are reproducible")
    parser.add_argument("--duration", type=float, default=0.0, metavar="S",
                        help="stop after S seconds (default: run until Ctrl-C)")
    parser.add_argument("--no-fft", action="store_true", help="omit /fft messages")
    parser.add_argument("--no-quality", action="store_true",
                        help="omit /quality/all messages")
    args = parser.parse_args(argv)

    if ":" in args.osc:
        host, _, port_text = args.osc.rpartition(":")
        host = host.strip("[]") or "127.0.0.1"
    else:
        host, port_text = "127.0.0.1", args.osc
    try:
        port = int(port_text)
    except ValueError:
        print("fake_dat: bad destination {!r}".format(args.osc), file=sys.stderr)
        return 2

    prefix = osc.normalize_prefix(args.prefix)
    join = lambda leaf: (prefix.rstrip("/") + "/" + leaf) if prefix != "/" else "/" + leaf

    addr_all = join("raw/all")
    addr_quality = join("quality/all")
    addr_gyro = (join("gyro/x"), join("gyro/y"))
    addr_battery = join("battery")
    addr_fft = [join("fft/" + name.lower()) for name in CHANNELS]

    rng = random.Random(args.seed)
    channels = [Channel(i, rng) for i in range(len(CHANNELS))]
    analyzers = [BandAnalyzer() for _ in CHANNELS]
    histories: List[List[float]] = [[] for _ in CHANNELS]

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dest = (host, port)
    period = 1.0 / args.rate

    print("fake_dat: streaming to {}:{}, prefix {}, {:.0f} Hz"
          .format(host, port, prefix, args.rate))
    print("fake_dat: messages tbugyq, timestamp timetag -- Ctrl-C to stop")

    start = time.time()
    sample_index = 0
    next_fft = 0.0
    # First reading a second in, not at t=0: until a battery frame arrives the
    # level is genuinely unknown, and epoc will not invent one.
    next_battery_frame = BATTERY_FRAME
    next_battery_drift = BATTERY_DRIFT
    last_battery_sent = None
    last_battery_time = 0.0
    # The counter cycles 0..127 and is replaced by a battery reading once a
    # second; quality refreshes on 34 of the 129 states. We do not reproduce
    # the selector table, only its rate.
    quality_period = 1.0 / 34.0
    next_quality = 0.0
    battery = 0.97
    blink_until = -1.0
    sent = 0

    try:
        while True:
            now = time.time()
            elapsed = now - start
            if args.duration and elapsed >= args.duration:
                break

            target = start + sample_index * period
            if target > now:
                time.sleep(min(target - now, 0.05))
                continue
            if now - target > 1.0:
                # Fell badly behind (the machine slept, say). Resynchronise
                # rather than sending a burst of backlogged samples.
                start = now - sample_index * period

            t = sample_index * period
            timetag = osc.unix_to_timetag(start + t)

            if elapsed > blink_until and rng.random() < 0.0015:
                blink_until = elapsed + rng.uniform(0.12, 0.25)
            blink = 0.0
            if elapsed <= blink_until:
                # A rounded bump rather than a spike, which is roughly what
                # an eye blink looks like on a frontal electrode.
                phase = 1.0 - (blink_until - elapsed) / 0.25
                blink = 90.0 * math.sin(math.pi * max(0.0, min(1.0, phase)))

            counts = []
            for i, channel in enumerate(channels):
                value = channel.sample(t, blink)
                counts.append(value)
                history = histories[i]
                history.append((value - channel.dc) * MICROVOLTS_PER_COUNT)
                if len(history) > FFT_WINDOW * 2:
                    del history[:FFT_WINDOW]

            sock.sendto(osc.encode_message(addr_all, [timetag] + counts), dest)
            sent += 1

            # Gyro: a slow wander plus noise, as two separate messages
            # sharing one timestamp, exactly as the DAT sends them.
            gx = int(round(22.0 * math.sin(2.0 * math.pi * 0.11 * t)
                           + 6.0 * math.sin(2.0 * math.pi * 0.4 * t) + rng.gauss(0, 1.5)))
            gy = int(round(16.0 * math.sin(2.0 * math.pi * 0.07 * t + 1.0)
                           + 4.0 * math.sin(2.0 * math.pi * 0.31 * t) + rng.gauss(0, 1.5)))
            sock.sendto(osc.encode_message(addr_gyro[0], [timetag, gx]), dest)
            sock.sendto(osc.encode_message(addr_gyro[1], [timetag, gy]), dest)
            sent += 2

            if not args.no_quality and t >= next_quality:
                next_quality = t + quality_period
                for channel in channels:
                    channel.step_quality()
                sock.sendto(osc.encode_message(
                    addr_quality, [timetag] + [int(c.quality) for c in channels]), dest)
                sent += 1

            if not args.no_fft and t >= next_fft and len(histories[0]) >= FFT_WINDOW:
                next_fft = t + FFT_INTERVAL
                for i, analyzer in enumerate(analyzers):
                    bands = analyzer.power(histories[i])
                    sock.sendto(osc.encode_message(
                        addr_fft[i], [timetag] + [float(v) for v in bands]), dest)
                    sent += 1

            if t >= next_battery_frame:
                # One synthetic battery frame per second, carrying epoc's own
                # trigger: send when the charge changed, or when the last send
                # is at least BATTERY_RESEND old. The charge itself only moves
                # once a minute, so most of these are resends -- which is the
                # point, since that is what a viewer joining mid-stream gets.
                next_battery_frame += BATTERY_FRAME
                if t >= next_battery_drift:
                    next_battery_drift += BATTERY_DRIFT
                    battery = max(0.0, battery - 0.01)
                if battery != last_battery_sent or t - last_battery_time >= BATTERY_RESEND:
                    sock.sendto(
                        osc.encode_message(addr_battery, [timetag, float(battery)]), dest)
                    last_battery_sent = battery
                    last_battery_time = t
                    sent += 1

            sample_index += 1
    except KeyboardInterrupt:
        pass
    finally:
        sock.close()

    span = time.time() - start
    print("\nfake_dat: {} messages in {:.1f} s ({:.0f} samples/s)"
          .format(sent, span, sample_index / span if span else 0.0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
