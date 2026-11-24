"""Write the Generative pack's generated files into pack/ (spec §18).

Every file this writes is made here from code: synth presets written field by field, and samples rendered by small
DSP routines with fixed random seeds, so the same script always writes the same bytes and the licence is ours
(pack/LICENSES.md), except the presets derived from factory ones (DERIVED): a factory file, checked by its hash, with
recorded edits, read from the factory card's files (--factory). Run it after changing a sound, then commit what
changed under pack/:

    python3 scripts/generative/make_pack.py            # writes into pack/
    python3 scripts/generative/make_pack.py --check    # exits 1 if pack/ differs from what it would write

--check also reports a file under pack/ that the script doesn't write (LICENSES.md aside): it would ship under the
CC0 claim. In the work tree it passes over what never ships: gitignored files (.DS_Store) and macOS's ._* AppleDouble
files. --pack DIR works on another copy of the pack; --committed checks it as a commit's tree, where every file ships,
so nothing is passed over (beta_release.py checks the committed tree this way).

Stdlib only.
"""

import argparse
import hashlib
import io
import math
import os
import random
import re
import struct
import subprocess
import sys
import wave
from typing import NamedTuple

RATE = 44100
# Files under pack/ that the script doesn't write but that belong there.
HAND_WRITTEN = {"LICENSES.md"}
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


# Ambient's soft one-shots (spec §18.2). Drums lanes are unpitched and never follow the key, so each is non-tonal:
# noise, a falling pitch or inharmonic partials, never one steady pitch.


def brush(seconds, seed, centre, swell, decay):
    """A brush on a snare: band-passed noise that swells in over `swell` seconds and dies away, with a faint wire
    rattle under it."""
    rng = random.Random(seed)
    svf = StateVariable(1.2)
    wires = StateVariable(0.7)
    n = int(seconds * RATE)
    out = []
    for i in range(n):
        t = i / RATE
        noise = rng.uniform(-1, 1)
        _, band, _ = svf.run(noise, centre * (1.0 + 0.3 * math.exp(-t * 8)))
        _, _, high = wires.run(noise, 5000)
        shape = min(1.0, t / swell) * math.exp(-max(0.0, t - swell) * decay)
        out.append((band + 0.25 * high) * shape)
    return out


def mallet(seconds, seed, start, end, drop):
    """A felt mallet on a drum: a sine whose pitch falls from `start` to `end` Hz (so it has no steady pitch), a soft
    noise thump at the attack, and a short decay."""
    rng = random.Random(seed)
    svf = StateVariable(0.8)
    n = int(seconds * RATE)
    out = []
    phase = 0.0
    for i in range(n):
        t = i / RATE
        phase += 2 * math.pi * (end + (start - end) * math.exp(-t * drop)) / RATE
        low, _, _ = svf.run(rng.uniform(-1, 1), 900)
        out.append(math.sin(phase) * math.exp(-t * 9) + 0.6 * low * math.exp(-t * 60))
    return out


def partials(t, phases, omegas, damping, divisors):
    """The sum at time `t` of sine partials: partial k at angular frequency omegas[k] from phases[k], scaled by
    damping(k, t) and divided by divisors[k] (wood() and ping())."""
    s = 0.0
    for k, omega in enumerate(omegas):
        s += math.sin(phases[k] + omega * t) * damping(k, t) / divisors[k]
    return s


def wood(seconds, seed, base, ratios):
    """A wood hit: a click and a few inharmonic modes above `base` Hz, each dying faster the higher it is."""
    rng = random.Random(seed)
    phases = [rng.random() * 2 * math.pi for _ in ratios]
    svf = StateVariable(0.9)
    omegas = [2 * math.pi * base * ratio for ratio in ratios]
    divisors = [k + 1 for k in range(len(ratios))]
    n = int(seconds * RATE)

    def damping(k, t):
        return math.exp(-t * (28 + 22 * k))

    out = []
    for i in range(n):
        t = i / RATE
        s = partials(t, phases, omegas, damping, divisors)
        _, band, _ = svf.run(rng.uniform(-1, 1), 3000)
        out.append(s + 0.5 * band * math.exp(-t * 400))
    return out


def ping(seconds, seed, base, ratios, decay):
    """An inharmonic metal ping: partials at non-integer `ratios` of `base` Hz, so no one pitch comes through, ringing
    out over `decay` seconds with a slow beating."""
    rng = random.Random(seed)
    phases = [rng.random() * 2 * math.pi for _ in ratios]
    omegas = [
        2 * math.pi * (base * ratio * (1.0 + 0.0015 * (k % 2)))
        for k, ratio in enumerate(ratios)
    ]
    divisors = [1 + 0.5 * k for k in range(len(ratios))]
    n = int(seconds * RATE)

    def damping(k, t):
        return math.exp(-t * (1 + 0.6 * k) / decay)

    out = []
    for i in range(n):
        t = i / RATE
        s = partials(t, phases, omegas, damping, divisors)
        out.append(s * min(1.0, t / 0.002))
    return out


SAMPLES = {
    "SAMPLES/FX/Riser/GEN Riser 1.wav": lambda: riser_noise(8.0, 1),
    "SAMPLES/FX/Riser/GEN Riser 2.wav": lambda: riser_saws(6.0, 2),
    "SAMPLES/FX/Riser/GEN Riser 3.wav": lambda: riser_tremolo(4.0, 3),
    # Dub's 1-bar Build lasts 3 s at 80 BPM, and about 2.2 s when BUILD INTO A DROP glides it up to Psytrance's
    # ~142 BPM: this one fits both (spec §10.5's Riser fit).
    "SAMPLES/FX/Riser/Short/GEN Riser Short.wav": lambda: riser_saws(2.0, 7),
    "SAMPLES/DRUMS/Ride/GEN Ride 1.wav": lambda: ride(1.6, 4, bell=False),
    "SAMPLES/DRUMS/Ride/GEN Ride 2.wav": lambda: ride(2.0, 5, bell=True),
    "SAMPLES/DRUMS/Clap/GEN Clap 1.wav": lambda: clap(
        0.45, 6, centre=1200, spacing=0.011
    ),
    "SAMPLES/DRUMS/Clap/GEN Clap 2.wav": lambda: clap(
        0.7, 7, centre=950, spacing=0.014
    ),
    "SAMPLES/DRUMS/Snare/GEN Brush 1.wav": lambda: brush(
        0.35, 8, centre=2400, swell=0.004, decay=14
    ),
    "SAMPLES/DRUMS/Snare/GEN Brush 2.wav": lambda: brush(
        0.6, 9, centre=1800, swell=0.06, decay=8
    ),
    "SAMPLES/DRUMS/Snare/GEN Brush 3.wav": lambda: brush(
        0.9, 10, centre=3200, swell=0.18, decay=6
    ),
    "SAMPLES/DRUMS/Other/GEN Mallet 1.wav": lambda: mallet(
        0.5, 11, start=210, end=90, drop=30
    ),
    "SAMPLES/DRUMS/Other/GEN Mallet 2.wav": lambda: mallet(
        0.5, 12, start=320, end=150, drop=25
    ),
    "SAMPLES/DRUMS/Other/GEN Mallet 3.wav": lambda: mallet(
        0.6, 13, start=150, end=60, drop=20
    ),
    "SAMPLES/DRUMS/Block/GEN Wood 1.wav": lambda: wood(
        0.25, 14, base=720, ratios=(1.0, 2.57, 4.13, 6.91)
    ),
    "SAMPLES/DRUMS/Block/GEN Wood 2.wav": lambda: wood(
        0.3, 15, base=480, ratios=(1.0, 2.31, 3.89, 5.73)
    ),
    "SAMPLES/DRUMS/Other/GEN Ping 1.wav": lambda: ping(
        1.6, 16, base=1130, ratios=(1.0, 1.59, 2.14, 2.83, 3.71), decay=0.5
    ),
    "SAMPLES/DRUMS/Other/GEN Ping 2.wav": lambda: ping(
        2.0, 17, base=1570, ratios=(1.0, 1.41, 2.29, 3.13, 4.37), decay=0.7
    ),
}


# ---- Synth presets ----


def knob(value):
    """A 0–50 sound editor value as the param it stores, as the menu saves it (computeFinalValueForStandardMenuItem):
    50 is INT32_MAX exactly, not 50 * 85899345 - 2**31, so a check against full sees a full knob as full (#483)."""
    if value == 50:
        return "0x7FFFFFFF"
    return f"0x{(value * 85899345 - 2**31) & 0xFFFFFFFF:08X}"


def cable(value):
    """A -50..50 patch cable amount as the param it stores."""
    return f"0x{(value * 21474836) & 0xFFFFFFFF:08X}"


def delay_feedbacks(xml):
    """Every delay feedback a preset's XML saves, in 0–50 menu units (knob()'s inverse, rounded): a sound's
    `<delayFeedback>` param and a kit's `<delay ... feedback="...">` attribute."""
    values = []
    params = re.findall(r"<delayFeedback>(0x[0-9A-Fa-f]{8})</delayFeedback>", xml)
    attributes = re.findall(r"<delay\b[^>]*\bfeedback=\"(0x[0-9A-Fa-f]{8})\"", xml)
    for raw in params + attributes:
        param = int(raw, 16)
        param -= 2**32 if param >= 2**31 else 0
        values.append(round((param + 2**31) / 85899345))
    return values


# Each repeat's gain is the menu value / 25 (dsp/delay/delay.cpp): at 25 or more an echo never dies in Perform, where
# the firmware's 15/16 clamp doesn't apply (spec §11.4).
RUNAWAY_DELAY_FEEDBACK = 25


def runaway_delay(xml):
    """Whether a preset saves a delay feedback whose echo never dies in Perform."""
    return any(value >= RUNAWAY_DELAY_FEEDBACK for value in delay_feedbacks(xml))


# The firmware's sidechain envelope rates (attackRateTable[6] * 4, releaseRateTable[12] * 8): attack about 0.5 ms,
# release about 64 ms at 44.1 kHz, unsynced.
SIDECHAIN_ATTACK_FAST = 386472
SIDECHAIN_RELEASE_64_MS = 2976


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
    osc2_volume=35,
    reverb=12,
    volume=36,
    lfo1_type=None,
    lfo1_rate=None,
    lfo2_type=None,
    lfo2_rate=None,
    unison=None,
    mod_fx=None,
    lpf_mode="24dB",
    hpf=None,
    fold=None,
    pulse_width=None,
    osc1_volume=50,
    delay=None,
    clipping=None,
    sidechain=None,
):
    """A subtractive synth preset. Fields left out take the firmware's defaults when it loads. `lfo1_type`
    and `lfo1_rate` (a knob) set LFO 1, unsynced; left out, LFO 1 keeps its defaults; `lfo2_type` and `lfo2_rate`
    LFO 2 (per voice) the same way. `unison` is (voices, detune, stereo spread); `mod_fx` is (type, rate knob, depth
    knob); `hpf`, `fold` (wavefolder) and `pulse_width` (both oscillators) are knobs, left out at the defaults.
    `delay` is a Delay (its feedback a knob, left out at 0); `clipping` the saturation (`clippingAmount`), left out at
    none. `sidechain` is the sound's own ducking envelope, (attack, release) as the firmware's rates, unsynced (a
    cable from `compressor` reads it)."""
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
    ]
    if clipping is not None:
        lines.append(f"\t<clippingAmount>{clipping}</clippingAmount>")
    if lfo1_type is not None:
        lines += [
            "\t<lfo1>",
            f"\t\t<type>{lfo1_type}</type>",
            "\t\t<syncLevel>0</syncLevel>",
            "\t</lfo1>",
        ]
    if lfo2_type is not None:
        lines += ["\t<lfo2>", f"\t\t<type>{lfo2_type}</type>", "\t</lfo2>"]
    lines += [
        "\t<mode>subtractive</mode>",
        f"\t<transpose>{transpose}</transpose>",
    ]
    if unison is not None:
        voices, detune, spread = unison
        lines += [
            "\t<unison>",
            f"\t\t<num>{voices}</num>",
            f"\t\t<detune>{detune}</detune>",
            f"\t\t<spread>{spread}</spread>",
            "\t</unison>",
        ]
    if sidechain is not None:
        lines += [
            "\t<compressor>",
            "\t\t<syncLevel>0</syncLevel>",
            f"\t\t<attack>{sidechain[0]}</attack>",
            f"\t\t<release>{sidechain[1]}</release>",
            "\t</compressor>",
        ]
    lines.append(f"\t<lpfMode>{lpf_mode}</lpfMode>")
    if mod_fx is not None:
        lines.append(f"\t<modFXType>{mod_fx[0]}</modFXType>")
    lines += delay.lines() if delay is not None else []
    lines += [
        "\t<defaultParams>",
        "\t\t<portamento>0x80000000</portamento>",
        f"\t\t<oscAVolume>{knob(osc1_volume)}</oscAVolume>",
        f"\t\t<oscBVolume>{knob(osc2_volume)}</oscBVolume>",
        f"\t\t<noiseVolume>{knob(noise)}</noiseVolume>",
        f"\t\t<volume>{knob(volume)}</volume>",
        "\t\t<pan>0x00000000</pan>",
        f"\t\t<lpfFrequency>{knob(lpf)}</lpfFrequency>",
        f"\t\t<lpfResonance>{knob(resonance)}</lpfResonance>",
        f"\t\t<hpfFrequency>{knob(hpf) if hpf is not None else '0x80000000'}</hpfFrequency>",
    ]
    if fold is not None:
        lines.append(f"\t\t<waveFold>{knob(fold)}</waveFold>")
    if pulse_width is not None:
        lines.append(f"\t\t<oscAPulseWidth>{knob(pulse_width)}</oscAPulseWidth>")
        lines.append(f"\t\t<oscBPulseWidth>{knob(pulse_width)}</oscBPulseWidth>")
    if lfo1_rate is not None:
        lines.append(f"\t\t<lfo1Rate>{knob(lfo1_rate)}</lfo1Rate>")
    if lfo2_rate is not None:
        lines.append(f"\t\t<lfo2Rate>{knob(lfo2_rate)}</lfo2Rate>")
    if mod_fx is not None:
        lines.append(f"\t\t<modFXRate>{knob(mod_fx[1])}</modFXRate>")
        lines.append(f"\t\t<modFXDepth>{knob(mod_fx[2])}</modFXDepth>")
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
        f"\t\t<delayFeedback>{knob(delay.feedback if delay is not None else 0)}</delayFeedback>",
        f"\t\t<reverbAmount>{knob(reverb)}</reverbAmount>",
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


class Delay(NamedTuple):
    """A preset's delay: ping-pong and analog on or off, the stock sync level and type (syncLevel 6 with syncType 19
    is a dotted 8th at the song's default magnitude), and its feedback as a knob."""

    ping_pong: bool
    analog: bool
    sync_level: int
    sync_type: int
    feedback: int

    def lines(self):
        return [
            "\t<delay>",
            f"\t\t<pingPong>{int(self.ping_pong)}</pingPong>",
            f"\t\t<analog>{int(self.analog)}</analog>",
            f"\t\t<syncLevel>{self.sync_level}</syncLevel>",
            f"\t\t<syncType>{self.sync_type}</syncType>",
            "\t</delay>",
        ]


# A dotted 8th, analog, ping-pong: Dub's echo (spec §4.5), and the Impact wash's and crack's.
DOTTED_EIGHTH = {"ping_pong": True, "analog": True, "sync_level": 6, "sync_type": 19}


def sample_preset(
    osc1,
    osc2=None,
    osc1_volume=50,
    osc2_volume=50,
    reverb=0,
    delay=None,
    clipping=None,
    velocity=8,
    volume=50,
):
    """A preset playing one or two factory samples once through (loopMode 1, ONCE), with an open filter, full
    sustain, decay and release (so the Decay sound control never reaches sustain, #483), the volume knob at `volume`
    (full by default), `clipping` as its saturation (`clippingAmount`) and a velocity cable of `velocity` to volume.
    Velocity is bipolar about 64, so a stronger cable makes a note over 64 louder than the knob's top. The samples
    are the factory card's (or a DERIVED copy in the pack): the preset names them."""
    oscillators = []
    for name, path in (("osc1", osc1), ("osc2", osc2)):
        if path is None:
            oscillators += [f"\t<{name}>", "\t\t<type>sine</type>", f"\t</{name}>"]
        else:
            oscillators += [
                f"\t<{name}>",
                "\t\t<type>sample</type>",
                "\t\t<loopMode>1</loopMode>",
                f"\t\t<fileName>{path}</fileName>",
                f"\t</{name}>",
            ]
    lines = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        "<firmwareVersion>c1.3.0</firmwareVersion>",
        "<sound>",
        *oscillators,
        "\t<polyphonic>auto</polyphonic>",
    ]
    if clipping is not None:
        lines.append(f"\t<clippingAmount>{clipping}</clippingAmount>")
    lines += [
        "\t<mode>subtractive</mode>",
        "\t<transpose>0</transpose>",
        "\t<lpfMode>24dB</lpfMode>",
    ]
    lines += delay.lines() if delay is not None else []
    lines += [
        "\t<defaultParams>",
        "\t\t<portamento>0x80000000</portamento>",
        f"\t\t<oscAVolume>{knob(osc1_volume)}</oscAVolume>",
        f"\t\t<oscBVolume>{knob(osc2_volume if osc2 is not None else 0)}</oscBVolume>",
        f"\t\t<noiseVolume>{knob(0)}</noiseVolume>",
        f"\t\t<volume>{knob(volume)}</volume>",
        "\t\t<pan>0x00000000</pan>",
        f"\t\t<lpfFrequency>{knob(50)}</lpfFrequency>",
        f"\t\t<lpfResonance>{knob(0)}</lpfResonance>",
        f"\t\t<hpfFrequency>{knob(0)}</hpfFrequency>",
        "\t\t<envelope1>",
        f"\t\t\t<attack>{knob(0)}</attack>",
        f"\t\t\t<decay>{knob(50)}</decay>",
        f"\t\t\t<sustain>{knob(50)}</sustain>",
        f"\t\t\t<release>{knob(50)}</release>",
        "\t\t</envelope1>",
        f"\t\t<delayFeedback>{knob(delay.feedback if delay is not None else 0)}</delayFeedback>",
        f"\t\t<reverbAmount>{knob(reverb)}</reverbAmount>",
        "\t\t<patchCables>",
        "\t\t\t<patchCable>",
        "\t\t\t\t<source>velocity</source>",
        "\t\t\t\t<destination>volume</destination>",
        f"\t\t\t\t<amount>{cable(velocity)}</amount>",
        "\t\t\t</patchCable>",
        "\t\t</patchCables>",
        "\t</defaultParams>",
        "</sound>",
        "",
    ]
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
    # The Drop's Impacts (#472, impact round 2): each plays out across the Drop's phrase (spec §10.5), with volume
    # and sustain full. The owner found round 2's levels too quiet, so each is about 6 dB louder, by a lever past
    # the volume knob's top measured in delugemu (Acid techno and Dub, the FX part alone). Saturation
    # (`clippingAmount`) caps a voice at a fixed level, under round 2's boom peak, so it lifts only the wash.
    # The boom: the factory 808 Kick sustained (a clean 1.5 s fall from about 100 Hz to 50 Hz) on both
    # oscillators, in phase: +6.0 dB. Impact round 3 (owner) took it 3 dB louder again everywhere it plays, by a
    # velocity cable of 30: +3.0 dB peak (the FX part alone) at an Impact's top velocity, 94; about +1.8 dB at 80.
    "SYNTHS/GENERATIVE/FX/GEN Impact Boom.XML": lambda: sample_preset(
        "SAMPLES/DRUMS/Kick/808 Kick sustained.wav",
        "SAMPLES/DRUMS/Kick/808 Kick sustained.wav",
        velocity=30,
    ),
    # Acid techno's boom: the same at a velocity cable of 18, +1.4 dB over round 2's build (peak, the FX part alone, at
    # velocity 94; about +0.8 dB at 80). At the others' +3 dB, Acid's Drop mix reached full scale (8 samples clipped,
    # seed 1, entropy 5), and at 22 (+2.0 dB) still at entropy 3 (2 samples); at 18 its Drops peak at -0.1 to -1.3 dBFS
    # over four Recipes (impact round 3, delugemu).
    "SYNTHS/GENERATIVE/FX/GEN Impact Boom Acid.XML": lambda: sample_preset(
        "SAMPLES/DRUMS/Kick/808 Kick sustained.wav",
        "SAMPLES/DRUMS/Kick/808 Kick sustained.wav",
        velocity=18,
    ),
    # The wash: white noise and a quiet square and a detuned saw a fifth over it (+7 st, +37 cents) through the
    # high-pass and an env2-swept resonant 24 dB low-pass, a 0.4 s splash (sustain 0), into a big reverb and a dotted
    # 8th analog ping-pong echo. Saturation 4: +6.1 dB on its first beat.
    "SYNTHS/GENERATIVE/FX/GEN Impact Wash.XML": lambda: preset(
        "square",
        "saw",
        noise=50,
        lpf=30,
        resonance=10,
        env1=(0, 28, 0, 28),
        env2=(0, 28, 0, 24),
        cables=[
            ("velocity", "volume", 8),
            ("velocity", "lpfFrequency", 10),
            ("envelope2", "lpfFrequency", 36),
        ],
        transpose=12,
        osc2_transpose=7,
        osc2_cents=37,
        osc2_volume=22,
        osc1_volume=24,
        reverb=44,
        volume=50,
        hpf=14,
        delay=Delay(**DOTTED_EIGHTH, feedback=24),
        clipping=4,
    ),
    # Dub's crack: the pack's DDD1 Snare wet (its leading silence cut, DERIVED) and the factory 808 Rim thrown into a
    # dotted 8th analog ping-pong echo and some reverb. Feedback 20: each repeat's gain is the menu value / 25 (0.8,
    # about -1.9 dB), so the echo dies within about 3 s at 72 BPM. At 25 or more it never dies in Perform (spec §11.4);
    # its first build's 26 held the echo at -23 to -26 dBFS for 53 repeats (Dub sign-off diagnosis). Its two oscillators
    # are taken, so a velocity cable of 50 lifts it: velocity is bipolar about 64, so at an Impact's 80-94 it adds about
    # 5.6 dB (peak); at low Energy, where steering pulls velocity toward 64 or below, it falls back to round 2's level
    # or under. Impact round 3 (owner) took it 3 dB quieter: the volume knob at 42 (-3.1 dB peak, the FX part alone).
    "SYNTHS/GENERATIVE/FX/GEN Dub Impact Crack.XML": lambda: sample_preset(
        "SAMPLES/DRUMS/Snare/Dub/GEN DDD1 Snare wet.wav",
        "SAMPLES/DRUMS/Rim/808 Rim.wav",
        osc1_volume=48,
        osc2_volume=44,
        reverb=24,
        delay=Delay(**DOTTED_EIGHTH, feedback=20),
        velocity=50,
        volume=42,
    ),
    # Dub's kick and crash: the factory 808 Kick and 808 Cymbal layered, into some reverb; velocity 50 as the crack's
    # (about 5.1 dB, peak).
    "SYNTHS/GENERATIVE/FX/GEN Impact KickCrash Dub.XML": lambda: sample_preset(
        "SAMPLES/DRUMS/Kick/808 Kick.wav",
        "SAMPLES/DRUMS/Crash/808 Cymbal.wav",
        osc1_volume=44,
        reverb=22,
        velocity=50,
    ),
    # Psytrance's rolling bass (spec §18.2): a saw and a square an octave under it into the resonant 24 dB low-pass,
    # sustain 0, so each 16th dies on its own at any gate; env2 closes the filter over the note for the pluck. Its
    # first tries (decay 2, filter 14, one saw at the preset volume 36) measured about 23 dB under the mix in delugemu
    # (#127 levels): the rate-table estimate overstated the decay. Decay 16 runs past the Loop's 20-tick gate at
    # 138-146 BPM (about 88 ms), so the gate's short release ends each note; the sub square and the full preset volume
    # carry the level. Velocity moves its volume. No reverb.
    "SYNTHS/GENERATIVE/GEN Psy Bass.XML": lambda: preset(
        "saw",
        "square",
        noise=0,
        lpf=32,
        resonance=20,
        env1=(0, 16, 0, 4),
        env2=(0, 12, 0, 4),
        cables=[("velocity", "volume", 30), ("envelope2", "lpfFrequency", 32)],
        osc2_transpose=-12,
        osc2_volume=40,
        reverb=0,
        volume=50,
    ),
    # Psytrance's rolling bass since Classic sign-off 1 (#313, #497): a sine sub and a saw top, both at the note (the
    # old preset's square an octave down sat at 19 Hz in D#), each 16th a short pluck: amp decay about 70 ms, sustain
    # 0, a short release; a fast env2 opening the resonant 24 dB low-pass for the "brrp", a little clipping for its
    # mids, the high-pass at about 30 Hz. Its own sidechain envelope (attack about 0.5 ms, release 64 ms: back within
    # the 105 ms 16th at 143 BPM) ducks it a little under the Kick, which sends at full (spec §10.4), while the bass
    # rests on the beat. Velocity moves its volume a little. No reverb. Set against delugemu captures (Song 3's Drop,
    # the Bass alone, notes/497.md): its 120 Hz-1 kHz band within 1 dB of the Arp's at 75 % (the old preset sat 3.7 dB
    # under; with the low-pass at 22 and no clipping this one sat 14.8 under), most of it from the clipping. Release 2:
    # at 3 the clipped tail of each bar's last 16th rang into the kick's 16th, 5 dB under it; at 2 it is 49 dB under.
    "SYNTHS/GENERATIVE/GEN Psy Roll Bass.XML": lambda: preset(
        "sine",
        "saw",
        noise=0,
        lpf=44,
        resonance=14,
        env1=(0, 11, 0, 2),
        env2=(0, 8, 0, 3),
        cables=[
            ("velocity", "volume", 20),
            ("envelope2", "lpfFrequency", 20),
            ("compressor", "volumePostReverbSend", 7),
        ],
        osc1_volume=50,
        osc2_volume=50,
        hpf=6,
        clipping=4,
        reverb=0,
        volume=50,
        sidechain=(SIDECHAIN_ATTACK_FAST, SIDECHAIN_RELEASE_64_MS),
    ),
    # The dub siren (spec §10.5, §18.2): a square and a saw a fifth apart, a triangle LFO sweeping the pitch up and
    # down about twice a second, a slow-ish attack and a long release that rings into its release Throw. Velocity
    # moves its volume.
    "SYNTHS/GENERATIVE/FX/GEN Siren.XML": lambda: preset(
        "square",
        "saw",
        noise=0,
        lpf=34,
        resonance=12,
        env1=(10, 20, 50, 30),
        env2=(0, 20, 50, 30),
        cables=[("velocity", "volume", 25), ("lfo1", "pitch", 35)],
        osc2_transpose=7,
        osc2_volume=30,
        reverb=20,
        lfo1_type="triangle",
        lfo1_rate=24,
    ),
    # Dub's bass (spec §18.2): a deep sine with a soft attack, a quiet triangle on top so it reads on small speakers,
    # the filter rounding it off, no reverb. It holds while the note does and lets go quickly, as the line moves on.
    # Velocity moves its volume.
    "SYNTHS/GENERATIVE/GEN Dub Sub.XML": lambda: preset(
        "sine",
        "triangle",
        noise=0,
        lpf=30,
        resonance=0,
        env1=(8, 24, 45, 14),
        env2=(0, 20, 50, 14),
        cables=[("velocity", "volume", 30)],
        osc2_volume=18,
        reverb=0,
        volume=50,
    ),
    # Dub's melodica (spec §18.2): a square reed and a saw a few cents apart, a breath of noise, a soft attack and
    # full sustain; velocity opens the filter a little as well as the volume.
    "SYNTHS/GENERATIVE/GEN Melodica.XML": lambda: preset(
        "square",
        "saw",
        noise=4,
        lpf=26,
        resonance=8,
        env1=(12, 22, 44, 16),
        env2=(4, 22, 40, 16),
        cables=[
            ("velocity", "volume", 30),
            ("velocity", "lpfFrequency", 12),
            ("envelope2", "lpfFrequency", 10),
        ],
        osc2_cents=6,
        osc2_volume=28,
        reverb=16,
    ),
    # House's chord stab (spec §18.2): a plain saw and square at the same pitch (no built-in interval, so a 7th or 9th
    # voicing stays clean), no portamento, sustain 0: each stab dies on its own over about a quarter of a second, the
    # Stab's 30-tick gate (~150 ms at 120-126 BPM) cutting it a little short. env2 closes the filter over the note for
    # the pluck. Velocity moves its volume and opens the filter a little.
    "SYNTHS/GENERATIVE/GEN House Stab.XML": lambda: preset(
        "saw",
        "square",
        noise=0,
        lpf=28,
        resonance=10,
        env1=(0, 22, 0, 12),
        env2=(0, 18, 0, 12),
        cables=[
            ("velocity", "volume", 30),
            ("velocity", "lpfFrequency", 10),
            ("envelope2", "lpfFrequency", 18),
        ],
        osc2_cents=5,
        osc2_volume=32,
        reverb=14,
    ),
    # Ambient's soft sub (spec §18.2): one sine, the filter open, no reverb. env1 attacks over about 80 ms (attack 29:
    # attackRateTable's 2487 a sample over the envelope's 2^23 is 3,373 samples), holds at full sustain and releases
    # over about 1.5 s (release 27: releaseRateTable's 124, 67,650 samples). Velocity moves its volume.
    "SYNTHS/GENERATIVE/GEN Sine Sub.XML": lambda: preset(
        "sine",
        "sine",
        noise=0,
        lpf=50,
        resonance=0,
        env1=(29, 20, 50, 27),
        env2=(0, 20, 50, 27),
        cables=[("velocity", "volume", 30)],
        osc2_volume=0,
        reverb=0,
    ),
    # ---- Ambient's Drone and Bass (#313; the owner's picks, sounds round 1) ----
    # The Drone holds the key's root and fifth (Register anchor C3) under the Pad. Each one: a slow attack and a long
    # release, the high-pass a little up and no oscillator under the note, so nothing much sounds under ~60 Hz, and
    # velocity moves the volume. LFO rates are knobs: 25 is about 1.25 Hz, each step about 1.25x.
    # Drone 1, tape strings: two saws a few cents apart, three unison voices spread wide, a soft 12 dB low-pass, a
    # breath of noise and a slow wow on the pitch. Static.
    "SYNTHS/GENERATIVE/GEN Ambient Drone 1.XML": lambda: preset(
        "saw",
        "saw",
        noise=5,
        lpf=28,
        resonance=4,
        env1=(40, 20, 50, 38),
        env2=(0, 20, 50, 38),
        cables=[("velocity", "volume", 25), ("lfo1", "pitch", 3)],
        osc2_cents=8,
        osc2_volume=30,
        reverb=26,
        volume=44,
        lfo1_type="triangle",
        lfo1_rate=22,
        unison=(3, 10, 30),
        lpf_mode="12dB",
        hpf=12,
    ),
    # Drone 2, breathing filter: a square and a saw an octave up into a resonant 12 dB low-pass that a slow sine LFO
    # opens and closes over about half a minute, while a per-voice LFO moves the square's pulse width. Evolving.
    "SYNTHS/GENERATIVE/GEN Ambient Drone 2.XML": lambda: preset(
        "square",
        "saw",
        noise=0,
        lpf=20,
        resonance=14,
        env1=(42, 20, 50, 40),
        env2=(0, 20, 50, 40),
        cables=[
            ("velocity", "volume", 25),
            ("velocity", "lpfFrequency", 8),
            ("lfo1", "lpfFrequency", 14),
            ("lfo2", "oscAPhaseWidth", 15),
        ],
        osc2_transpose=12,
        osc2_volume=22,
        reverb=28,
        lfo1_type="sine",
        lfo1_rate=9,
        lfo2_type="triangle",
        lfo2_rate=12,
        unison=(2, 6, 40),
        lpf_mode="12dB",
        hpf=10,
    ),
    # Drone 3, glass fifth: a sine with a quiet sine an octave and a fifth over it, which a slow LFO fades in and out;
    # two unison voices spread fully apart, more reverb. Evolving, the purest of the four.
    "SYNTHS/GENERATIVE/GEN Ambient Drone 3.XML": lambda: preset(
        "sine",
        "sine",
        noise=0,
        lpf=40,
        resonance=0,
        env1=(40, 20, 50, 40),
        env2=(0, 20, 50, 40),
        cables=[("velocity", "volume", 25), ("lfo1", "oscBVolume", -16)],
        osc2_transpose=19,
        osc2_volume=26,
        reverb=30,
        volume=44,
        lfo1_type="sine",
        lfo1_rate=11,
        unison=(2, 5, 50),
        hpf=12,
    ),
    # The Bass plays the chord root (the key's root F#2-F3, the Drone's octave) held for two bars. Each one: a soft attack, full-ish
    # sustain, a release under a second, no reverb to speak of, and harmonics over the root so a small speaker plays
    # it. Velocity moves the volume.
    # Bass 2, felt pulse: a square with a slightly detuned second square into a 12 dB low-pass, the second envelope
    # blooming the filter over the first half second and settling; velocity opens the filter too.
    "SYNTHS/GENERATIVE/GEN Ambient Bass 2.XML": lambda: preset(
        "square",
        "square",
        noise=2,
        lpf=24,
        resonance=6,
        env1=(31, 30, 46, 27),
        env2=(24, 34, 18, 28),
        cables=[
            ("velocity", "volume", 30),
            ("velocity", "lpfFrequency", 8),
            ("envelope2", "lpfFrequency", 14),
        ],
        osc2_cents=5,
        osc2_volume=28,
        reverb=4,
        volume=48,
        lpf_mode="12dB",
    ),
    # Bass 3, folded sine: a sine with a quiet saw an octave up through a soft low-pass and the wavefolder, which adds
    # odd harmonics; velocity folds it further, so harder notes read brighter.
    "SYNTHS/GENERATIVE/GEN Ambient Bass 3.XML": lambda: preset(
        "sine",
        "saw",
        noise=0,
        lpf=30,
        resonance=0,
        env1=(30, 30, 48, 27),
        env2=(0, 20, 50, 27),
        cables=[("velocity", "volume", 30), ("velocity", "waveFold", 10)],
        osc2_transpose=12,
        osc2_volume=22,
        reverb=0,
        volume=48,
        lpf_mode="12dB",
        fold=20,
    ),
    # Bass 4, tape bass: a saw and a triangle into a darker 24 dB low-pass with a slow filter bloom, a little noise and
    # a slow wobble on the pitch.
    "SYNTHS/GENERATIVE/GEN Ambient Bass 4.XML": lambda: preset(
        "saw",
        "triangle",
        noise=6,
        lpf=21,
        resonance=8,
        env1=(30, 30, 46, 27),
        env2=(28, 36, 24, 30),
        cables=[
            ("velocity", "volume", 30),
            ("velocity", "lpfFrequency", 6),
            ("envelope2", "lpfFrequency", 12),
            ("lfo1", "pitch", 2),
        ],
        osc2_volume=40,
        reverb=4,
        volume=48,
        lfo1_type="triangle",
        lfo1_rate=20,
        hpf=8,
    ),
}


# ---- Presets derived from the factory card ----
#
# A pack preset close to a factory one is the factory file with a few recorded edits, never a hand-edited copy: the
# script reads the source from the factory card's files (--factory), checks it is the file the edits were written
# against (its SHA-256), applies the edits and writes the result. These files are derived from Synthstrom Audible's
# factory content, so they are not CC0: pack/LICENSES.md lists each under "Derived from the factory presets", and
# --check fails while one is missing there. To add one: a DERIVED entry (source, its hash, the edits), a row in that
# section, then run the script.

# The factory card's files, as delugemu's factory image holds them (~/source/delugemu-sd/sdcard, spec §21.3).
FACTORY = os.environ.get(
    "DELUGE_FACTORY_CARD",
    os.path.join(os.path.expanduser("~"), "source", "delugemu-sd", "sdcard"),
)
LICENSES = "LICENSES.md"
DERIVED_HEADING = "## Derived from the factory presets"


class Field:
    """Set the element <tag> (exactly one in the file) to `value`."""

    def __init__(self, tag, value):
        self.tag, self.value = tag, value

    def apply(self, text):
        pattern = re.compile(rf"(<{self.tag}>)[^<]*(</{self.tag}>)")
        return replace_once(pattern, rf"\g<1>{self.value}\g<2>", text, f"<{self.tag}>")


class Cable:
    """Set the amount of the patch cable from `source` to `destination` (exactly one in the file) to a -50..50
    amount."""

    def __init__(self, source, destination, amount):
        self.source, self.destination, self.amount = source, destination, amount

    def apply(self, text):
        pattern = re.compile(
            rf"(<source>{self.source}</source>\s*<destination>{self.destination}</destination>\s*<amount>)"
            r"[^<]*(</amount>)"
        )
        what = f"the {self.source} -> {self.destination} cable"
        return replace_once(pattern, rf"\g<1>{cable(self.amount)}\g<2>", text, what)


class Transpose:
    """Set the transpose of `element` (osc1, osc2, modulator1, modulator2: exactly one in the file) to `semitones`."""

    def __init__(self, element, semitones):
        self.element, self.semitones = element, semitones

    def apply(self, text):
        pattern = re.compile(
            rf"(<{self.element}>(?:(?!</{self.element}>).)*?<transpose>)[^<]*(</transpose>)",
            re.DOTALL,
        )
        what = f"the {self.element} transpose"
        return replace_once(pattern, rf"\g<1>{self.semitones}\g<2>", text, what)


class LeadTrim:
    """Cut a WAV's first `frames` frames (its leading silence) and fade what is left in over `fade` frames, against
    a click; the format (channels, width, rate) stays the source's. 16-bit PCM only. Extra chunks are dropped."""

    binary = True

    def __init__(self, frames, fade):
        self.frames, self.fade = frames, fade

    def apply(self, data):
        with wave.open(io.BytesIO(data)) as w:
            channels, width, rate = w.getnchannels(), w.getsampwidth(), w.getframerate()
            count = w.getnframes()
            raw = w.readframes(count)
        if width != 2:
            raise ValueError(f"{width * 8}-bit samples: LeadTrim reads 16-bit PCM only")
        if self.frames + self.fade >= count:
            raise ValueError(
                f"cutting {self.frames} frames and fading {self.fade} leaves nothing of {count} frames"
            )
        samples = list(struct.unpack(f"<{count * channels}h", raw))[
            self.frames * channels :
        ]
        for i in range(self.fade * channels):
            samples[i] = int(samples[i] * (i // channels) / self.fade)
        out = io.BytesIO()
        with wave.open(out, "wb") as w:
            w.setnchannels(channels)
            w.setsampwidth(width)
            w.setframerate(rate)
            w.writeframes(struct.pack(f"<{len(samples)}h", *samples))
        return out.getvalue()


class TailFade:
    """Keep a WAV's first `hold_ms`, then fade it out to silence over a raised-cosine to `end_ms` and cut it there;
    the format (channels, width, rate) stays the source's. 16- and 24-bit PCM. Extra chunks are dropped."""

    binary = True

    def __init__(self, hold_ms, end_ms):
        self.hold_ms, self.end_ms = hold_ms, end_ms

    def apply(self, data):
        with wave.open(io.BytesIO(data)) as w:
            channels, width, rate = w.getnchannels(), w.getsampwidth(), w.getframerate()
            count = w.getnframes()
            raw = w.readframes(count)
        if width not in (2, 3):
            raise ValueError(
                f"{width * 8}-bit samples: TailFade reads 16- or 24-bit PCM only"
            )
        hold = round(rate * self.hold_ms / 1000)
        end = round(rate * self.end_ms / 1000)
        if not 0 < hold < end < count:
            raise ValueError(
                f"holding {self.hold_ms} ms and ending at {self.end_ms} ms doesn't fit {count} frames"
            )
        out = bytearray()
        for frame in range(end):
            gain = (
                1.0
                if frame < hold
                else 0.5 * (1 + math.cos(math.pi * (frame - hold) / (end - hold)))
            )
            for c in range(channels):
                at = (frame * channels + c) * width
                sample = int.from_bytes(raw[at : at + width], "little", signed=True)
                out += round(sample * gain).to_bytes(width, "little", signed=True)
        buffer = io.BytesIO()
        with wave.open(buffer, "wb") as w:
            w.setnchannels(channels)
            w.setsampwidth(width)
            w.setframerate(rate)
            w.writeframes(bytes(out))
        return buffer.getvalue()


def replace_once(pattern, replacement, text, what):
    new, count = pattern.subn(replacement, text)
    if count != 1:
        raise ValueError(f"{what}: {count} matches, expected 1")
    return new


class Derived:
    """A pack preset made from factory file `source` (relative to the card root, with SHA-256 `sha256`) by `edits`."""

    def __init__(self, source, sha256, *edits):
        self.source, self.sha256, self.edits = source, sha256, edits

    def make(self, factory):
        path = os.path.join(factory, self.source)
        if not os.path.isfile(path):
            raise ValueError(f"{path}: not on the factory card (--factory)")
        with open(path, "rb") as f:
            data = f.read()
        digest = hashlib.sha256(data).hexdigest()
        if digest != self.sha256:
            raise ValueError(
                f"{path}: SHA-256 {digest}, the edits were written against {self.sha256}"
            )
        if not self.edits:
            raise ValueError(
                f"{self.source}: no edits, so the pack would ship a plain factory copy"
            )
        binary = [getattr(edit, "binary", False) for edit in self.edits]
        if any(binary) and not all(binary):
            raise ValueError(
                f"{self.source}: a mix of sample (binary) and preset (text) edits"
            )
        if all(binary):
            for edit in self.edits:
                data = edit.apply(data)
            return data
        text = data.decode("utf-8")
        for edit in self.edits:
            text = edit.apply(text)
        return text.encode("utf-8")


# Dub techno's Stab (#322): the three factory chord presets with their own echo off (delay feedback 0), so the part's
# delay setup and its Throws alone make the echo (spec §10.6). Their volume knobs set each where its original sat
# against the rest of Dub techno, which plays at 76 percent of its presets' volume (techno.cpp's kDubRest, about
# -3.9 dB): 037 at the knob's top is 3.9 dB under the original with its echo, 082 at 34 and 072 at 28 are about as
# far under theirs (delugemu captures, the Stab alone, entropy 2, seed 1).
DERIVED = {
    "SYNTHS/GENERATIVE/GEN Echo Chord Dry.XML": Derived(
        "SYNTHS/037 Echo Chord.XML",
        "4fbfc11f1977f933d7c8e3ff9cb02c399390769aa30ccae7569c43401be76143",
        Field("delayFeedback", knob(0)),
        Field("volume", knob(50)),
    ),
    "SYNTHS/GENERATIVE/GEN Short Sharp Dry.XML": Derived(
        "SYNTHS/082 Short Sharp Delay.XML",
        "6500c76753252c52a497d04139bf4dff9dbb01adf6b998a43375b866cc2bd4c2",
        Field("delayFeedback", knob(0)),
        Field("volume", knob(34)),
    ),
    "SYNTHS/GENERATIVE/GEN Kyoto Phono Dry.XML": Derived(
        "SYNTHS/072 Kyoto Phono.XML",
        "690716bab5fbe4835d3d7e925bf1d9b9cd666fc3ff8280e44815b2b70bb40c46",
        Field("delayFeedback", knob(0)),
        Field("volume", knob(28)),
    ),
    # Dub techno's Pad: 126 an octave up (the owner's Techno listening answers, DI3). The Pad plays an octave under
    # the Pad Register's default, under the Stab's chord, where 126 (its second carrier already at -17) sounded an
    # octave too low. Both carriers and both modulators move, so the FM ratios and the timbre stay.
    "SYNTHS/GENERATIVE/GEN Dark FM Pad High.XML": Derived(
        "SYNTHS/126 Dark FM Pad.XML",
        "f6aee81d39281b7393975f491f79cddc2abfeeb7b1d19b2ac8b73ce3afcceb0f",
        Transpose("osc1", 12),
        Transpose("osc2", -5),
        Transpose("modulator1", -12),
        Transpose("modulator2", 16),
    ),
    # Dub's swap snare and its Impact crack (Dub sign-off 1): the factory DDD1 Snare wet with its leading 32.7 ms cut.
    # The factory file is near silent until its attack at frame 1464 (33.2 ms), so on the backbeat it sounded 33 ms
    # behind the kick and hats it lands with, a flam with the lazy +2 lean on top (every other Dub snare file starts
    # within 0.4 ms). 24 frames (0.5 ms) stay before the attack, faded in over 22. A folder of its own, so no lane's
    # tags reach it: only Dub's swap names it, by its path.
    "SAMPLES/DRUMS/Snare/Dub/GEN DDD1 Snare wet.wav": Derived(
        "SAMPLES/DRUMS/Snare/DDD1 Snare wet.wav",
        "56ad8d3411d31ddd026caade6d1e15e0ac31210ad923932841a0582578e7f15d",
        LeadTrim(1440, 22),
    ),
    # Psytrance's Lead (Classic sign-off 1, #497): 057 FM Lead with its first modulator an octave lower (+12, not +24)
    # and its low-pass at 18 (was about 24). 057's top notes were shrill: with the modulator at four times the note, a
    # 1.2 kHz D#6 put its sidebands at about 4-8 kHz, and every pitch's band over 4 kHz sat within 2-4 dB of its whole
    # level. Halving its Brightness range alone moved that band by under 1 dB; this copy puts it 7.6-8.2 dB under
    # (delugemu captures, notes/497.md).
    "SYNTHS/GENERATIVE/GEN FM Lead Soft.XML": Derived(
        "SYNTHS/057 FM Lead.XML",
        "842d4f6ccabc77c834f5c60985175db9730335fc215a818cbd1c49e70157cdcb",
        Transpose("modulator1", 12),
        Field("lpfFrequency", knob(18)),
    ),
    # Psytrance's kicks (Classic sign-off 1, #497): Dean Daughters' Psy set's three, each held for 60 ms and faded out
    # by 130 ms (a raised cosine), so the tail is 20 dB down by 110-115 ms (195-255 ms in the factory files) and gone
    # before the rolling bass's first 16th (the e, from 105 ms at 143 BPM) speaks: it sat 2-6 dB under the beat there
    # and masked the bass. A folder of their own and a tag no factory file holds (PSYKICK-): the Psy kit and
    # Psytrance's Kick lane list name it. Without the pack the lane list's Deep Sky Kick HQ plays instead.
    **{
        f"SAMPLES/DRUMS/Kick/Psy/GEN PSYKICK-{name}.wav": Derived(
            f"SAMPLES/Artists/Dean Daughters/Psy/KICK-{name}.wav",
            sha,
            TailFade(60, 130),
        )
        for name, sha in (
            (
                "Bonus01",
                "459f95e312938dcb3dd3755c835e8ba1894bad9922989ef114e49cf8a53fe42c",
            ),
            (
                "Radar",
                "44e1761f6faa6ab3403b9a1c5baf832c771ade43b0e6213ced0585052e0e9c57",
            ),
            (
                "Silence",
                "1aaaad60a8199951878401c26af4d314097dcef6ac68a50a15e9df9fdd001326",
            ),
        )
    },
}


def licence_gaps(pack):
    """DERIVED files that pack/LICENSES.md's derived section doesn't list with their source, as messages."""
    path = os.path.join(pack, LICENSES)
    text = ""
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            text = f.read()
    section = (
        text.split(DERIVED_HEADING, 1)[1].split("\n## ", 1)[0]
        if DERIVED_HEADING in text
        else ""
    )
    gaps = []
    for target, derived in DERIVED.items():
        rows = [line for line in section.splitlines() if f"`{target}`" in line]
        if not any(f"`{derived.source}`" in row for row in rows):
            gaps.append(
                f'{target} (from {derived.source}): no row in {LICENSES}\'s "{DERIVED_HEADING[3:]}"'
            )
    return gaps


def git_ignored(pack, paths):
    """Those of `paths` (absolute) that git ignores where they are; none outside a git work tree."""
    if not paths:
        return set()
    proc = subprocess.run(
        ["git", "check-ignore", "--stdin", "-z"],
        cwd=pack,
        input="\0".join(paths) + "\0",
        capture_output=True,
        text=True,
        check=False,
    )
    return set(proc.stdout.split("\0")) - {""} if proc.returncode == 0 else set()


def unexpected_files(pack, written, committed=False):
    """Files under pack that are neither written by the script nor hand-written, as sorted /-separated paths. Unless
    `committed` (pack is a commit's tree, where every file ships), ._* AppleDouble files and gitignored files are
    passed over: they never reach a commit."""
    found = {}
    for folder, _, names in os.walk(pack):
        for name in names:
            if not committed and name.startswith("._"):
                continue
            path = os.path.join(folder, name)
            rel = os.path.relpath(path, pack).replace(os.sep, "/")
            if rel not in written and rel not in HAND_WRITTEN:
                found[path] = rel
    if not committed:
        for path in git_ignored(pack, list(found)):
            found.pop(path, None)
    return sorted(found.values())


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--check", action="store_true", help="only report whether pack/ is up to date"
    )
    parser.add_argument(
        "--pack", default=PACK, help="the pack folder (default: the repo's pack/)"
    )
    parser.add_argument(
        "--committed",
        action="store_true",
        help="--pack is a commit's tree: every file counts, gitignored and ._* ones too",
    )
    parser.add_argument(
        "--factory",
        default=FACTORY,
        help="the factory card's files, the derived presets' sources (default: $DELUGE_FACTORY_CARD, else "
        "~/source/delugemu-sd/sdcard); without them --check only checks those presets are there",
    )
    args = parser.parse_args(argv)
    files = {**SAMPLES, **PRESETS}
    if os.path.isdir(args.factory):
        files.update(
            {path: (lambda d=d: d.make(args.factory)) for path, d in DERIVED.items()}
        )
    elif not args.check:
        print(f"no factory card at {args.factory}: give --factory", file=sys.stderr)
        return 1
    else:
        print(
            f"no factory card at {args.factory}: the derived presets are checked for presence only",
            file=sys.stderr,
        )
    stale = []
    for path in DERIVED:
        if path not in files and not os.path.exists(os.path.join(args.pack, path)):
            stale.append(path)
    for path, make in files.items():
        try:
            data = make()
        except ValueError as e:
            print(e, file=sys.stderr)
            return 1
        data = to_wav(data) if path in SAMPLES else data
        target = os.path.normpath(os.path.join(args.pack, path))
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
    unexpected = (
        unexpected_files(args.pack, {**files, **DERIVED}, args.committed)
        if args.check
        else []
    )
    gaps = licence_gaps(args.pack)
    for gap in gaps:
        print("unlisted: " + gap)
    for path in unexpected:
        print(
            "unexpected: "
            + path
            + " (the script doesn't write it: delete it, or make it in the script)"
        )
    return 1 if (args.check and (stale or unexpected)) or gaps else 0


if __name__ == "__main__":
    sys.exit(main())
