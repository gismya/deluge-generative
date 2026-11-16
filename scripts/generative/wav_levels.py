"""Levels of WAV captures (genre tuning, spec §21.5): RMS and peak in dBFS and the count of samples at full scale,
in pure Python (no numpy). With --full, each other capture is taken as the full mix with a part gated out, and the
missing part's level is estimated by energy subtraction: its mean square is the full mix's minus the capture's, which
holds for parts that don't correlate. That suits a gen_emu.py capture A/B (`capture --mute PART`), which plays the
same piece with and without the part.

    python3 scripts/generative/wav_levels.py [--skip SECONDS] [--full FULL.wav] CAPTURE.wav...

Reads 16-, 24- and 32-bit integer PCM, any channel count; every channel's samples count alike. A 180 s stereo capture
takes about 30 s.
"""

import argparse
import array
import math
import sys
import wave
from dataclasses import dataclass


@dataclass
class Level:
    mean_square: float  # of full scale
    peak: float  # of full scale
    full_scale: int  # samples at the top or bottom code
    frames: int

    @property
    def rms_db(self):
        return db_power(self.mean_square)

    @property
    def peak_db(self):
        return 20 * math.log10(self.peak) if self.peak > 0 else -math.inf


def db_power(mean_square):
    return 10 * math.log10(mean_square) if mean_square > 0 else -math.inf


def samples(raw, width):
    """The signed integers in little-endian PCM `raw` of `width` bytes each."""
    if width == 2:
        data = array.array("h")
    elif width == 4:
        data = array.array("i")
    elif width == 3:
        # Sign-extend each 3-byte sample into a 4-byte one.
        wide = bytearray()
        for i in range(0, len(raw) - 2, 3):
            wide += raw[i : i + 3] + (b"\xff" if raw[i + 2] & 0x80 else b"\x00")
        raw, data = bytes(wide), array.array("i")
    else:
        raise ValueError(f"{8 * width}-bit samples")
    data.frombytes(raw[: len(raw) - len(raw) % data.itemsize])
    if sys.byteorder == "big":
        data.byteswap()
    return data


def measure(path, skip=0.0):
    """The Level of the WAV at `path`, leaving out its first `skip` seconds."""
    with wave.open(path, "rb") as w:
        width, rate = w.getsampwidth(), w.getframerate()
        w.readframes(int(skip * rate))
        frames = w.getnframes() - min(w.getnframes(), int(skip * rate))
        data = samples(w.readframes(frames), width)
    top = 2 ** (8 * width - 1) - 1
    energy, peak, clipped = 0.0, 0, 0
    for s in data:
        energy += s * s
        a = -s if s < 0 else s
        peak = max(peak, a)
        if a >= top:
            clipped += 1
    count = max(len(data), 1)
    return Level(energy / count / (top * top), peak / top, clipped, frames)


def missing_part(full, without):
    """(The gated-out part's RMS in dBFS, its level against the rest in dB), estimated from the full mix and the
    capture without it; None when the capture without it isn't quieter."""
    part = full.mean_square - without.mean_square
    if part <= 0 or without.mean_square <= 0:
        return None
    return db_power(part), db_power(part) - db_power(without.mean_square)


def describe(name, level):
    return f"{name}: rms {level.rms_db:6.1f} dBFS  peak {level.peak_db:6.2f} dBFS  full-scale {level.full_scale}"


def main(argv=None, out=print):
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument(
        "--skip",
        type=float,
        default=0.0,
        help="seconds to leave out at the start of each file",
    )
    p.add_argument(
        "--full",
        help="the full mix: estimate each other file's missing part against it",
    )
    p.add_argument("wavs", nargs="*", help="captures to measure")
    args = p.parse_args(argv)
    if not args.wavs and not args.full:
        p.error("give at least one WAV")
    try:
        full = measure(args.full, args.skip) if args.full else None
        if full is not None:
            out(describe(args.full, full))
        for path in args.wavs:
            level = measure(path, args.skip)
            line = describe(path, level)
            if full is not None:
                estimate = missing_part(full, level)
                line += (
                    "  | missing part: not quieter than the full mix"
                    if estimate is None
                    else f"  | missing part ~{estimate[0]:6.1f} dBFS, {estimate[1]:+5.1f} dB against the rest"
                )
            out(line)
    except (wave.Error, EOFError, ValueError) as exc:
        sys.exit(f"wav_levels: {exc}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
