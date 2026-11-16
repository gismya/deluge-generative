"""Write the Generative pack's generated files into pack/ (spec §18).

Every file this writes is made here from code: synth presets written field by field, and samples rendered by small
DSP routines with fixed random seeds, so the same script always writes the same bytes and the licence is ours
(pack/LICENSES.md). Run it after changing a sound, then commit what changed under pack/:

    python3 scripts/generative/make_pack.py            # writes into pack/
    python3 scripts/generative/make_pack.py --check    # exits 1 if pack/ differs from what it would write

Stdlib only.
"""

import argparse
import io
import math
import os
import random
import struct
import sys
import wave

RATE = 44100
PACK = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "pack")


# ---- Samples ----


def to_wav(samples):
    """16-bit mono WAV bytes, normalised to -1 dBFS peak, with a 2 ms fade at both ends against clicks."""
    peak = max(abs(s) for s in samples) or 1.0
    gain = 10 ** (-1 / 20) / peak
    fade = int(RATE * 0.002)
    n = len(samples)
    frames = bytearray()
    for i, s in enumerate(samples):
        edge = min(1.0, i / fade, (n - 1 - i) / fade)
        frames += struct.pack("<h", round(max(-1.0, min(1.0, s * gain * edge)) * 32767))
    out = io.BytesIO()
    with wave.open(out, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(bytes(frames))
    return out.getvalue()


class StateVariable:
    """Chamberlin state-variable filter with a cutoff that may change every sample."""

    def __init__(self, q):
        self.damp = 1.0 / q
        self.low = 0.0
        self.band = 0.0

    def run(self, x, cutoff):
        f = 2 * math.sin(math.pi * min(cutoff, RATE / 6) / RATE)
        self.low += f * self.band
        high = x - self.low - self.damp * self.band
        self.band += f * high
        return self.low, self.band, high


def rise(t, length, power):
    """0 → 1 over the file, bending upward with `power`."""
    return (t / length) ** power


def riser_noise(seconds, seed):
    """Noise through a resonant band-pass sweeping from 200 Hz to 9 kHz, swelling into the file end."""
    rng = random.Random(seed)
    svf = StateVariable(4.0)
    n = int(seconds * RATE)
    out = []
    for i in range(n):
        t = i / RATE
        cutoff = 200 * (9000 / 200) ** rise(t, seconds, 1.4)
        _, band, high = svf.run(rng.uniform(-1, 1), cutoff)
        out.append((band + 0.15 * high) * (0.05 + rise(t, seconds, 2.2)))
    return out


def riser_saws(seconds, seed):
    """Three detuned saws climbing three octaves, a low-pass opening, swelling into the file end."""
    rng = random.Random(seed)
    phases = [rng.random() for _ in range(3)]
    detune = [1.0, 1.006, 0.994]
    svf = StateVariable(1.5)
    n = int(seconds * RATE)
    out = []
    for i in range(n):
        t = i / RATE
        pitch = 110 * 8 ** rise(t, seconds, 1.6)
        s = 0.0
        for k in range(3):
            phases[k] = (phases[k] + pitch * detune[k] / RATE) % 1.0
            s += 2 * phases[k] - 1
        low, _, _ = svf.run(s / 3, 300 + 12000 * rise(t, seconds, 1.8))
        out.append(low * (0.1 + rise(t, seconds, 1.5)))
    return out


def riser_tremolo(seconds, seed):
    """High-passed noise and a rising sine, chopped by a tremolo that speeds from 2 Hz to 32 Hz."""
    rng = random.Random(seed)
    svf = StateVariable(0.9)
    n = int(seconds * RATE)
    out = []
    phase = 0.0
    trem = 0.0
    for i in range(n):
        t = i / RATE
        x = rise(t, seconds, 1.0)
        phase += 2 * math.pi * (220 * 4**x) / RATE
        trem += 2 * math.pi * (2 * 16**x) / RATE
        _, _, high = svf.run(rng.uniform(-1, 1), 300 + 6000 * x * x)
        gate = 0.5 + 0.5 * math.sin(trem)
        out.append(
            (0.6 * high + 0.4 * math.sin(phase)) * gate * (0.1 + rise(t, seconds, 2.0))
        )
    return out


def ride(seconds, seed, bell):
    """A metallic ride: inharmonic square partials and noise, high-passed, with a long decay. `bell` adds a cup."""
    rng = random.Random(seed)
    freqs = [f * 2.2 for f in (205.3, 304.4, 369.6, 522.7, 540.0, 800.0)]
    phases = [rng.random() for _ in freqs]
    svf = StateVariable(0.8)
    n = int(seconds * RATE)
    out = []
    for i in range(n):
        t = i / RATE
        metal = 0.0
        for k, f in enumerate(freqs):
            phases[k] = (phases[k] + f / RATE) % 1.0
            metal += 1.0 if phases[k] < 0.5 else -1.0
        _, _, high = svf.run(metal / len(freqs) + 0.5 * rng.uniform(-1, 1), 6500)
        s = high * (0.25 * math.exp(-t * 40) + math.exp(-t * 2.6))
        if bell:
            s += 0.35 * math.sin(2 * math.pi * 2450 * t) * math.exp(-t * 3.5)
        out.append(s)
    return out


def clap(seconds, seed, centre, spacing):
    """Band-passed noise: three quick bursts, then a decaying tail, like a drum machine's clap."""
    rng = random.Random(seed)
    svf = StateVariable(2.5)
    n = int(seconds * RATE)
    out = []
    for i in range(n):
        t = i / RATE
        bursts = sum(
            math.exp(-(t - k * spacing) * 180) for k in range(3) if t >= k * spacing
        )
        tail = math.exp(-(t - 3 * spacing) * 14) if t >= 3 * spacing else 0.0
        _, band, _ = svf.run(rng.uniform(-1, 1), centre)
        out.append(band * (bursts + 0.8 * tail))
    return out


SAMPLES = {
    "SAMPLES/FX/Riser/GEN Riser 1.wav": lambda: riser_noise(8.0, 1),
    "SAMPLES/FX/Riser/GEN Riser 2.wav": lambda: riser_saws(6.0, 2),
    "SAMPLES/FX/Riser/GEN Riser 3.wav": lambda: riser_tremolo(4.0, 3),
    "SAMPLES/DRUMS/Ride/GEN Ride 1.wav": lambda: ride(1.6, 4, bell=False),
    "SAMPLES/DRUMS/Ride/GEN Ride 2.wav": lambda: ride(2.0, 5, bell=True),
    "SAMPLES/DRUMS/Clap/GEN Clap 1.wav": lambda: clap(
        0.45, 6, centre=1200, spacing=0.011
    ),
    "SAMPLES/DRUMS/Clap/GEN Clap 2.wav": lambda: clap(
        0.7, 7, centre=950, spacing=0.014
    ),
}


# ---- Synth presets ----


def knob(value):
    """A 0–50 sound editor value as the param it stores (getParamFromUserValue's default branch)."""
    return f"0x{(value * 85899345 - 2**31) & 0xFFFFFFFF:08X}"


def cable(value):
    """A -50..50 patch cable amount as the param it stores."""
    return f"0x{(value * 21474836) & 0xFFFFFFFF:08X}"


def preset(
    osc1,
    osc2,
    noise,
    lpf,
    resonance,
    env1,
    env2,
    cables,
    transpose=0,
    osc2_transpose=0,
    osc2_cents=0,
):
    """A subtractive synth preset. Fields left out take the firmware's defaults when it loads."""
    lines = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        "<firmwareVersion>c1.3.0</firmwareVersion>",
        "<sound>",
        "\t<osc1>",
        f"\t\t<type>{osc1}</type>",
        "\t</osc1>",
        "\t<osc2>",
        f"\t\t<type>{osc2}</type>",
        f"\t\t<transpose>{osc2_transpose}</transpose>",
        f"\t\t<cents>{osc2_cents}</cents>",
        "\t</osc2>",
        "\t<polyphonic>auto</polyphonic>",
        "\t<mode>subtractive</mode>",
        f"\t<transpose>{transpose}</transpose>",
        "\t<lpfMode>24dB</lpfMode>",
        "\t<defaultParams>",
        "\t\t<portamento>0x80000000</portamento>",
        "\t\t<oscAVolume>0x7FFFFFFF</oscAVolume>",
        f"\t\t<oscBVolume>{knob(35)}</oscBVolume>",
        f"\t\t<noiseVolume>{knob(noise)}</noiseVolume>",
        f"\t\t<volume>{knob(36)}</volume>",
        "\t\t<pan>0x00000000</pan>",
        f"\t\t<lpfFrequency>{knob(lpf)}</lpfFrequency>",
        f"\t\t<lpfResonance>{knob(resonance)}</lpfResonance>",
        "\t\t<hpfFrequency>0x80000000</hpfFrequency>",
    ]
    for name, (attack, decay, sustain, release) in (
        ("envelope1", env1),
        ("envelope2", env2),
    ):
        lines += [
            f"\t\t<{name}>",
            f"\t\t\t<attack>{knob(attack)}</attack>",
            f"\t\t\t<decay>{knob(decay)}</decay>",
            f"\t\t\t<sustain>{knob(sustain)}</sustain>",
            f"\t\t\t<release>{knob(release)}</release>",
            f"\t\t</{name}>",
        ]
    lines += [
        "\t\t<delayFeedback>0x80000000</delayFeedback>",
        f"\t\t<reverbAmount>{knob(12)}</reverbAmount>",
    ]
    lines.append("\t\t<patchCables>")
    for source, destination, amount in cables:
        lines += [
            "\t\t\t<patchCable>",
            f"\t\t\t\t<source>{source}</source>",
            f"\t\t\t\t<destination>{destination}</destination>",
            f"\t\t\t\t<amount>{cable(amount)}</amount>",
            "\t\t\t</patchCable>",
        ]
    lines += ["\t\t</patchCables>", "\t</defaultParams>", "</sound>", ""]
    return "\n".join(lines).encode()


PRESETS = {
    # Swells over a few seconds: both envelopes attack slowly, env2 opens the filter and lifts the pitch.
    "SYNTHS/GENERATIVE/FX/GEN Riser.XML": lambda: preset(
        "saw",
        "saw",
        noise=30,
        lpf=12,
        resonance=20,
        env1=(44, 20, 50, 22),
        env2=(46, 20, 50, 22),
        cables=[
            ("velocity", "volume", 25),
            ("envelope2", "lpfFrequency", 40),
            ("envelope2", "pitch", 18),
        ],
        osc2_transpose=12,
        osc2_cents=7,
    ),
    # Starts bright and high, then env2's decay drags pitch and filter down as the note dies away.
    "SYNTHS/GENERATIVE/FX/GEN Fall.XML": lambda: preset(
        "saw",
        "square",
        noise=15,
        lpf=22,
        resonance=15,
        env1=(0, 42, 0, 30),
        env2=(0, 44, 0, 30),
        cables=[
            ("velocity", "volume", 25),
            ("envelope2", "pitch", 35),
            ("envelope2", "lpfFrequency", 30),
        ],
        osc2_transpose=-12,
    ),
    # A boom: a sine an octave down with a fast pitch drop and a noise crack on top.
    "SYNTHS/GENERATIVE/FX/GEN Impact.XML": lambda: preset(
        "sine",
        "sine",
        noise=35,
        lpf=20,
        resonance=10,
        env1=(0, 38, 0, 35),
        env2=(0, 18, 0, 18),
        cables=[
            ("velocity", "volume", 30),
            ("envelope2", "pitch", 40),
            ("envelope2", "lpfFrequency", 40),
        ],
        transpose=-12,
        osc2_transpose=-12,
    ),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--check", action="store_true", help="only report whether pack/ is up to date"
    )
    args = parser.parse_args()
    stale = []
    for path, make in list(SAMPLES.items()) + list(PRESETS.items()):
        data = make()
        data = to_wav(data) if path.endswith(".wav") else data
        target = os.path.normpath(os.path.join(PACK, path))
        current = None
        if os.path.exists(target):
            with open(target, "rb") as f:
                current = f.read()
        if current == data:
            continue
        stale.append(path)
        if not args.check:
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with open(target, "wb") as f:
                f.write(data)
    for path in stale:
        print(("stale: " if args.check else "wrote: ") + path)
    return 1 if args.check and stale else 0


if __name__ == "__main__":
    sys.exit(main())
