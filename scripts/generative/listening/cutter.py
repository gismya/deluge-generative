"""Cutting clips: from a capture or rough render WAV, by bar at its tempo, to 16 bits with 10 ms fades, then AAC m4a.

A capture's audio starts at PLAY, the harness's bar 0, and a bar lasts 240 / BPM seconds. Each bar line rounds to its
nearest frame, so clips cut back to back tile exactly. The level is kept: it is part of what the owner judges.
"""

import array
import os
import subprocess
import tempfile
import wave

FADE_S = 0.01
AAC_BITRATE = 160000  # bits a second, unless the content file gives a clip its own kbps


def window(tempo, first_bar, end_bar, rate, total=None):
    """(first frame, frame count) of bars [first_bar, end_bar) at `tempo` BPM, stopping at `total` frames if given."""
    start = round(first_bar * 240.0 * rate / tempo)
    end = round(end_bar * 240.0 * rate / tempo)
    if total is not None:
        start, end = min(start, total), min(end, total)
    return start, max(0, end - start)


def to_16_bit(frames, sample_width, channels, rate):
    """16-bit PCM bytes from 16- or 32-bit PCM (32-bit shifted down, no level change), faded in and out over 10 ms."""
    if sample_width == 4:
        a = array.array("h", (x >> 16 for x in array.array("i", frames)))
    elif sample_width == 2:
        a = array.array("h", frames)
    else:
        raise ValueError(f"{8 * sample_width}-bit samples: only 16 and 32")
    n = len(a) // channels
    fade = min(int(FADE_S * rate), n // 2)
    for i in range(fade):
        g = i / fade
        for c in range(channels):
            a[i * channels + c] = int(a[i * channels + c] * g)
            a[(n - 1 - i) * channels + c] = int(a[(n - 1 - i) * channels + c] * g)
    return a.tobytes()


def cut_wav(src, dest_wav, tempo, first_bar, end_bar):
    """Writes bars [first_bar, end_bar) of `src` to `dest_wav` (16-bit, faded). Returns the clip's seconds."""
    with wave.open(src) as w:
        rate, ch, sw = w.getframerate(), w.getnchannels(), w.getsampwidth()
        start, count = window(tempo, first_bar, end_bar, rate, total=w.getnframes())
        if count == 0:
            raise ValueError(
                f"{src}: bars {first_bar}-{end_bar} at {tempo:g} BPM start past its end"
            )
        w.setpos(start)
        frames = w.readframes(count)
    with wave.open(dest_wav, "wb") as o:
        o.setnchannels(ch)
        o.setsampwidth(2)
        o.setframerate(rate)
        o.writeframes(to_16_bit(frames, sw, ch, rate))
    return count / rate


def cut(src, dest_m4a, tempo, first_bar, end_bar, scratch=None, bitrate=None):
    """Cuts bars [first_bar, end_bar) of `src` to an AAC m4a with afconvert, at `bitrate` bits a second (default
    AAC_BITRATE). Returns the clip's seconds."""
    with tempfile.TemporaryDirectory(dir=scratch) as tmp:
        wav = os.path.join(tmp, "cut.wav")
        seconds = cut_wav(src, wav, tempo, first_bar, end_bar)
        os.makedirs(os.path.dirname(os.path.abspath(dest_m4a)), exist_ok=True)
        subprocess.run(
            [
                "afconvert",
                "-f",
                "m4af",
                "-d",
                "aac",
                "-b",
                str(bitrate or AAC_BITRATE),
                wav,
                dest_m4a,
            ],
            check=True,
            capture_output=True,
        )
    return seconds
