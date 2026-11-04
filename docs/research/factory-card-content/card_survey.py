"""Survey the factory SD card for Generative mode palettes (issue #34).

Reads SYNTHS/*.XML and SAMPLES/ from a Deluge card and prints Markdown tables.
It only parses files; it never executes anything from the card.

    python3 -I card_survey.py <card root> synths    # per-preset table: features, roles, flags
    python3 -I card_survey.py <card root> counts    # summary counts behind the doc's numbers
    python3 -I card_survey.py <card root> fmfilter  # what each FM preset saved in its filter section
    python3 -I card_survey.py <card root> drums     # audio files per SAMPLES/DRUMS folder
    python3 -I card_survey.py <card root> fxshape fx_candidates.txt  # loudness/brightness over time
    python3 -I card_survey.py <card root> samples   # every audio file with duration
    python3 -I card_survey.py <card root> json      # all synth features as JSON

Roles come from REVIEW (a hand review of each preset); role_fit() is only the starting rule.

Value conversions follow the firmware at v1.3.0-rc3 (paths relative to src/deluge/):
- menu value 0..50 = (raw + 2^31) / 85899345 (util/functions.cpp:1359-1389 getParamFromUserValue)
- env times: Envelope::render advances pos by the final param value per sample until
  pos >= 2^23 (modulation/envelope.cpp:29-118). Final value for decay/release is
  neutral * lookupReleaseRate(raw * range >> 32) >> 32, neutral 140<<9 (release) or
  70<<9 (decay), range 2^30 (functions.cpp:57-163, 249-253, 1340-1356). Attack is
  getExp(4096, -(raw * 1.5 * 2^29 >> 32)) = 4096 * 2^(-x / 2^26) (functions.cpp:503-512).
- polyphonic: "mono", "auto"/"0", "legato", "choke"/"2", anything else POLY
  (functions.cpp:854-876).
- arp: old files store <arpeggiator><mode>; any mode other than "off" turns the arp on
  (modulation/arpeggiator.cpp:1751-1763, functions.cpp:980-1048). Synth presets load it
  into SoundInstrument::defaultArpSettings (processing/sound/sound_instrument.cpp:85).
- FM presets saved before community 1.2.0 load with LPF and HPF off
  (processing/sound/sound.cpp:3261-3265).
"""

import collections
import itertools
import json
import math
import os
import struct
import sys
import xml.etree.ElementTree as ET

SR = 44100
U = 85899345
RELEASE_TABLE_64 = [
    1959518848,
    240577040,
    126456408,
    84972672,
    63518640,
    50408868,
    41567836,
    35202512,
    30400724,
    26649400,
    23637820,
    21166812,
    19102814,
    17352918,
    15850494,
    14546511,
    13404089,
    12394955,
    11497068,
    10692993,
    9968758,
    9313033,
    8716538,
    8171592,
    7671792,
    7211749,
    6786901,
    6393359,
    6027784,
    5687299,
    5369406,
    5071928,
    4792960,
    4530827,
    4284050,
    4051316,
    3831462,
    3623446,
    3426337,
    3239300,
    3061582,
    2892503,
    2731448,
    2577859,
    2431229,
    2291096,
    2157036,
    2028664,
    1905625,
    1787593,
    1674270,
    1565377,
    1460662,
    1359888,
    1262836,
    1169305,
    1079105,
    992062,
    908014,
    826807,
    748301,
    672363,
    598870,
    527703,
    458757,
]
AUDIO_EXT = (
    ".wav",
    ".aif",
    ".aiff",
)  # util/functions.cpp:1327-1333 isAudioFilename (also skips dotfiles)


def s32(x):
    x &= 0xFFFFFFFF
    return x - (1 << 32) if x & 0x80000000 else x


def raw(text):
    if text is None:
        return None
    text = text.strip()
    return s32(int(text, 16)) if text.lower().startswith("0x") else int(text)


def menu(v):
    return None if v is None else round((v + 2**31) / U)


def mul32(a, b):
    return (a * b) >> 32


def lookup_release_rate(inp):
    which = (inp >> 24) + 32
    further = (inp << 7) & 0x7FFFFFFF
    if which < 0:
        return RELEASE_TABLE_64[0]
    if which >= 64:
        return RELEASE_TABLE_64[64]
    v1, v2 = RELEASE_TABLE_64[which], RELEASE_TABLE_64[which + 1]
    return (mul32(v2, further) + mul32(v1, 0x7FFFFFFF - further)) << 1


def release_seconds(v, neutral=140 << 9):
    final = mul32(neutral, lookup_release_rate(mul32(v, 1 << 30)))
    return (1 << 23) / max(final, 1) / SR


def attack_seconds(v):
    x = mul32(v, 805306368)
    final = 4096 * 2 ** (-x / 2**26)
    return (1 << 23) / final / SR


def load_xml(path):
    with open(path, encoding="utf-8", errors="replace") as fh:
        txt = fh.read()
    if txt.lstrip().startswith("<?xml"):
        txt = txt.split("?>", 1)[1]
    return ET.fromstring("<file>" + txt + "</file>")


def txt(e, path, default=None):
    f = e.find(path)
    return f.text.strip() if f is not None and f.text and f.text.strip() else default


def voice_mode(p):
    if p in ("mono",):
        return "MONO"
    if p in ("auto", "0"):
        return "AUTO"
    if p == "legato":
        return "LEGATO"
    if p in ("choke", "2"):
        return "CHOKE"
    return "POLY"


def synth_features(path):
    root = load_xml(path)
    s = root.find("sound")
    dp = s.find("defaultParams")
    g = lambda tag: raw(txt(dp, tag))
    f = {"file": os.path.basename(path)}
    f["fw"] = txt(root, "firmwareVersion", "none")
    f["mode"] = txt(s, "mode", "subtractive")
    f["osc"] = [txt(s, "osc1/type"), txt(s, "osc2/type")]
    f["osc_vol"] = [menu(g("oscAVolume")), menu(g("oscBVolume"))]
    f["noise"] = menu(g("noiseVolume"))
    f["voice"] = voice_mode(txt(s, "polyphonic", "poly"))
    f["unison"] = int(txt(s, "unison/num", "1"))
    f["detune"] = int(txt(s, "unison/detune", "0"))
    f["transpose"] = int(txt(s, "transpose", "0"))
    f["osc_transpose"] = [
        int(txt(s, "osc1/transpose", "0")),
        int(txt(s, "osc2/transpose", "0")),
    ]
    f["sync"] = txt(s, "osc2/oscillatorSync", "0") == "1"
    f["mod_transpose"] = [
        int(txt(s, "modulator1/transpose", "0")),
        int(txt(s, "modulator2/transpose", "0")),
    ]
    f["mod_vol"] = [menu(g("modulator1Amount")), menu(g("modulator2Amount"))]
    arp_mode = txt(s, "arpeggiator/mode") or txt(s, "arpeggiator/arpMode")
    f["arp"] = arp_mode if arp_mode and arp_mode != "off" else None
    f["arp_oct"] = txt(s, "arpeggiator/numOctaves")
    e1 = dp.find("envelope1")
    f["atk_s"] = attack_seconds(raw(txt(e1, "attack")))
    f["dec_s"] = release_seconds(raw(txt(e1, "decay")), 70 << 9)
    f["sus"] = menu(raw(txt(e1, "sustain")))
    f["rel_s"] = release_seconds(raw(txt(e1, "release")))
    f["rel_menu"] = menu(raw(txt(e1, "release")))
    f["lpf_mode"] = txt(s, "lpfMode", "24dB")
    f["lpf"] = menu(g("lpfFrequency"))
    f["lpf_res"] = menu(g("lpfResonance"))
    f["hpf"] = menu(g("hpfFrequency"))
    f["porta"] = menu(g("portamento"))
    f["reverb"] = menu(g("reverbAmount"))
    f["delay_fb"] = menu(g("delayFeedback"))
    f["modfx"] = txt(s, "modFXType", "none")
    f["modfx_depth"] = menu(g("modFXDepth"))
    f["clip"] = int(txt(s, "clippingAmount", "0"))
    f["bitcrush"] = menu(g("bitCrush"))
    f["srr"] = menu(g("sampleRateReduction"))
    f["stutter"] = menu(g("stutterRate"))
    f["volume"] = menu(g("volume"))
    cables = []
    for pc in dp.findall("patchCables/patchCable"):
        amt = raw(txt(pc, "amount", "0"))
        cables.append(
            (txt(pc, "source"), txt(pc, "destination"), round(amt / 2**30 * 100))
        )
    f["cables"] = cables
    f["knobs"] = [
        txt(k, "controlsParam")
        for k in s.findall("modKnobs/modKnob")
        if txt(k, "patchAmountFromSource") is None
    ]
    f["samples"] = sorted(
        {txt(r, "fileName") for r in s.iter("sampleRange") if txt(r, "fileName")}
        | {txt(s, f"osc{i}/fileName") for i in (1, 2) if txt(s, f"osc{i}/fileName")}
    )
    return f


def has_cable(f, src=None, dst=None, min_abs=1):
    return any(
        (src is None or c[0] == src)
        and (dst is None or c[1] == dst)
        and abs(c[2]) >= min_abs
        for c in f["cables"]
    )


def fm_filter_use(f):
    """What a factory FM preset has saved in its (dead) filter section."""
    if f["mode"] != "fm":
        return []
    out = []
    if f["lpf"] is not None and f["lpf"] < 50:
        out.append(f"LPF {f['lpf']}")
    if f["hpf"] is not None and f["hpf"] > 0:
        out.append(f"HPF {f['hpf']}")
    for c in f["cables"]:
        if (
            c[1] in ("lpfFrequency", "hpfFrequency", "lpfResonance", "hpfResonance")
            and c[2]
        ):
            out.append(f"{c[0]}→{c[1]} {c[2]:+d}%")
    for k in f["knobs"]:
        if k in ("lpfFrequency", "lpfResonance", "hpfFrequency", "hpfResonance"):
            out.append(f"knob {k}")
    return out


PITCH_DESTS = ("pitch", "oscAPitch", "oscBPitch")


def fight_flags(f):
    """Things in the saved preset that would fight the Generator (see the doc, section 2)."""
    flags = []
    if f["arp"]:
        flags.append("ARP")  # clip copies it on load; plays its own notes
    if f["rel_s"] >= 2.0:
        flags.append(f"rel {f['rel_s']:.0f}s")  # tails overlap the next notes/chords
    if f["reverb"] >= 20:
        flags.append(f"rev {f['reverb']}")
    if f["delay_fb"] >= 20:
        flags.append(f"dly {f['delay_fb']}")
    if f["modfx"] != "none" and f["modfx_depth"] >= 30:
        flags.append(f"{f['modfx']} {f['modfx_depth']}")
    if f["clip"] >= 5:
        flags.append(f"sat {f['clip']}")
    if max(f["bitcrush"], f["srr"]) >= 5:
        flags.append("crush")
    strong = [
        c
        for c in f["cables"]
        if c[0] in ("lfo1", "lfo2", "random")
        and c[1] in PITCH_DESTS
        and abs(c[2]) >= (15 if c[1] == "pitch" else 40)
        and not (c[1] == "oscBPitch" and f["sync"])
    ]  # on a synced osc2 that's a timbre sweep
    if strong:
        flags.append("pitch wobble")  # LFO/random moves pitch far enough to blur a line
    return flags


def key_offsets(f):
    """(semitones from the played note, menu volume) for each audible oscillator or FM carrier.
    Ringmod multiplies the two oscillators, so both always count there (volumes are ignored).
    Osc2 hard-synced to osc1 sounds at osc1's pitch, so it doesn't count."""
    out = []
    for i in (0, 1):
        vol = 50 if f["mode"] == "ringmod" else (f["osc_vol"][i] or 0)
        if vol > 0 and not (i == 1 and f.get("sync")):
            out.append((f["transpose"] + f["osc_transpose"][i], vol))
    return out


def key_note(f):
    """How the preset's oscillator tuning sits against the note the Generator plays.
    '' = every audible oscillator on the played pitch class (or a quiet high harmonic).
    'off-key +N' = no audible oscillator on the played pitch class: every note sounds N semitones off.
    'adds 5th' etc. = a second audible oscillator stacks that interval on every note."""
    oscs = key_offsets(f)
    pcs = {st % 12 for st, _ in oscs}
    if not oscs or pcs == {0}:
        return ""
    if f["mode"] == "ringmod":
        return "ring (inharmonic)"
    if 0 not in pcs:
        shift = min(st for st, _ in oscs) % 12
        return f"off-key +{shift}"
    root = min(st for st, _ in oscs if st % 12 == 0)
    names = {
        7: "5th",
        5: "5th",
        3: "3rd",
        4: "3rd",
        8: "3rd",
        9: "3rd",
        1: "2nd",
        2: "2nd",
        10: "2nd",
        11: "2nd",
        6: "tritone",
    }
    extra = sorted(
        {
            names[st % 12]
            for st, vol in oscs
            if st % 12 and not (vol <= 16 and st - root >= 24)
        }
    )  # quiet high harmonic = timbre
    return f"adds {'/'.join(extra)}" if extra else ""


def pitch_offset(f):
    # Sound transpose plus the main oscillator's transpose (carrier/osc1).
    return f["transpose"] + f["osc_transpose"][0]


def role_fit(f):
    """Rule-of-thumb role fit from the envelope and register (hand review in ROLE_REVIEW overrides it)."""
    p, atk, sus, dec, rel = (
        pitch_offset(f),
        f["atk_s"],
        f["sus"],
        f["dec_s"],
        f["rel_s"],
    )
    plucky = sus <= 25 and dec <= 2.5
    roles = []
    if p <= -12 and atk <= 0.03 and rel < 2.0:
        roles.append("Bass")
    if atk <= 0.06 and -12 < p + 12 and rel < 1.6 and (plucky or sus >= 20):
        roles.append("Chords")
    if atk >= 0.1 or (sus >= 30 and rel >= 1.0):
        roles.append("Pad")
    if atk <= 0.1 and p >= -12 and rel < 2.0:
        roles.append("Lead")
    return roles


# Hand review: the rule fit above, corrected by reading each preset's oscillators, cables, FX and name.
# Roles: Bass, Chords, Pad, Lead; FX lanes: Riser, Fall, Impact; '–' = no role (avoid).
# First role listed is the best fit.
REVIEW = {
    0: ("Bass", "unison saw + square sub, env2 filter pluck"),
    1: ("Bass", "LFO sweeps the synced osc2: moving timbre"),
    2: ("Bass", "round square, 1.3 s release"),
    3: ("Bass", "unison square/saw"),
    4: ("Bass", "dub bass by name; env2 on osc A, light chorus"),
    5: ("Bass", "mono 4-voice unison, dark filter"),
    6: ("Bass", "saved legato + portamento"),
    7: ("Bass", "plain detuned saw"),
    8: ("Bass", "8-voice FM unison, feedback envelope: gritty"),
    9: ("Bass Lead", "rave hoover, legato + portamento saved"),
    10: ("Bass", "rough: LFO on osc2 pitch, random FM feedback"),
    11: ("Bass", "pitch + noise envelope growl, −36 st"),
    12: ("Bass", "sync sweep stab"),
    13: ("Bass", "808-style long decay, legato"),
    14: ("Bass Impact", "808 boom with 3 s tail, saturated"),
    15: ("Bass Lead", "resonant (42) ringmod: acid-capable"),
    16: ("Bass", "saturated, 3.3 s tail"),
    17: ("Lead Chords", "bright saw"),
    18: ("Lead", "0.2 s soft attack"),
    19: ("Pad", "string pad, 2 s release"),
    20: ("Lead Pad", "soft saw, 2 s release"),
    21: ("Lead", "plain square lead"),
    22: ("Lead", "filter-LFO lead, 3 s release"),
    23: ("Bass", "root + fifth, LFO filter wow"),
    24: ("Lead", "pluck with a 6 s tail"),
    25: ("Chords", "soft organ pluck"),
    26: ("Chords", "pulse organ: stab or skank"),
    27: ("Lead Chords", "pulse-width envelope"),
    28: ("Chords Lead", "PWM"),
    29: ("Lead", "chip lead under a saved arp"),
    30: ("Lead", "portamento lead with delay"),
    31: ("Chords Pad", "choir-ish, short release"),
    32: ("Pad", "ringmod choir, HPF"),
    33: ("Lead Chords", "ringmod square"),
    34: ("Pad Chords", "square choir, 1.9 s release"),
    35: ("Lead Bass", "mono 8-voice unison bell"),
    36: ("Pad Lead", "ambient square, 2.2 s release"),
    37: ("Chords", "fifth on every note + delay: dub-techno chord"),
    38: ("Lead", "chorused pluck, 2.4 s tail"),
    39: ("Lead Bass", "ringmod saws"),
    40: ("–", "whole sound a semitone sharp"),
    41: ("Lead", "vibrato pluck, 3 s tail"),
    42: ("Lead", "pure high triangle"),
    43: ("Lead", "plain square"),
    44: ("Lead", "chip lead, pitch blip"),
    45: ("Lead", "sync lead"),
    46: ("Lead Bass", "sync lead, an octave down"),
    47: ("Bass", "plain dirty bass"),
    48: ("Bass", "thin pulse, −36 st"),
    49: ("Lead Chords", "plain FM"),
    50: ("Bass", "plain FM bass"),
    51: ("Lead", "brass; carrier 2 adds a major 10th"),
    52: ("Pad", "soft FM, 4.3 s release"),
    53: ("Pad", "horn swell, 0.4 s attack, reverb"),
    54: ("Pad Lead", "sine swell that fades"),
    55: ("Lead", "theremin, portamento saved"),
    56: ("Pad", "FM bell drone, 10 s release"),
    57: ("Lead", "FM lead"),
    58: ("Lead Chords", "FM pluck"),
    59: ("Lead", "FM guitar, saturated"),
    60: ("–", "whole sound a fifth off (−5 st)"),
    61: ("Lead", "breathy panpipe, noise 50"),
    62: ("Lead", "trumpet: dub horn"),
    63: ("Bass", "tuba"),
    64: ("Lead", "reed: melodica stand-in, reverb"),
    65: ("Pad", "cello; carrier 2 adds a major 2nd"),
    66: ("Lead Pad", "violin"),
    67: ("Lead", "marimba: mallet pluck"),
    68: ("Lead", "FM bell"),
    69: ("Lead", "FM bell"),
    70: ("Lead", "ringmod glockenspiel, 2 s tail"),
    71: ("Chords Lead", "Rhodes"),
    72: ("Lead", "FM pluck with delay"),
    73: ("Chords Lead", "piano"),
    74: ("Chords Lead", "electric piano, flanger"),
    75: ("Pad Chords", "EP + strings, 2.2 s release"),
    76: ("Chords", "drawbar-style organ"),
    77: ("Chords", "percussive organ: skank"),
    78: ("Chords", "house organ stab (sustain 0)"),
    79: ("Bass", "mono −24 st, noise"),
    80: ("Bass Chords", "house organ bass (osc2 +19 st)"),
    81: ("Impact", "FM boom, 13.6 s release"),
    82: ("Chords Lead", "short stab into delay: skank"),
    83: ("Chords Pad", "chorused ringmod"),
    84: ("–", "whole sound shifted 8 st down"),
    85: ("Bass Lead", "fizzy, bitcrushed"),
    86: ("Chords", "organ stab"),
    87: ("Lead", "saturated mono lead, flanger"),
    88: ("Chords", "ringmod stab with a pitch yelp"),
    89: ("Lead", "degraded lead"),
    90: ("Chords Pad", "FM organ"),
    91: ("Lead", "FM pluck; adds a major 10th"),
    92: ("Lead", "tremolo FM; adds a major 10th"),
    93: ("Lead", "low FM bell"),
    94: ("Lead", "ambient lead: phaser, crush, 3 s tail"),
    95: ("Bass Lead", "FM feedback grit"),
    96: ("Lead", "power chord on every note"),
    97: ("Lead", "saturated resonant filter: acid-capable"),
    98: ("Lead", "saturated sync"),
    99: ("Bass", "Reese: saturated ringmod + chorus"),
    100: ("Lead", "bitcrushed noise lead"),
    101: ("Lead", "chip lead, fifth stacked"),
    102: ("–", "whole sound a fifth off"),
    103: ("Fall", "pitch falls with the amp envelope over ~19 s"),
    104: ("–", "semitone cluster (carrier 2 at −1)"),
    105: ("Bass", "ringmod: difference tone an octave down"),
    106: ("Lead", "FM hang drum, 3 s tail"),
    107: ("–", "tritone carrier, random pitch"),
    108: ("Lead", "robot lead under a saved arp, crushed"),
    109: ("Lead", "talking filter, fifth stacked"),
    110: ("Lead", "glassy ringmod, delay + chorus + crush"),
    111: ("–", "siren: LFO on both osc pitches, saved arp"),
    112: ("Impact", "noise + pitch-drop hit, saved arp, 15 s release"),
    113: ("–", "random pitch chaos, saved arp"),
    114: ("Pad", "FM swell"),
    115: ("Lead", "crushed lead"),
    116: ("Pad Riser", "noise envelope, 5.8 s release"),
    117: ("Lead", "adds a minor 3rd (major 6th below)"),
    118: ("Pad", "3 s attack, 4.8 s release, delay"),
    119: ("Pad", "3 s attack, heavy reverb + delay"),
    120: ("Pad Chords", "sustained harsh unison, short release"),
    121: ("Lead", "bell lead under a saved arp, reverb"),
    122: ("Pad", "carrier 2 adds a 9th"),
    123: ("Pad", "FM shimmer, flanger"),
    124: ("Pad", "LFO filter pad"),
    125: ("Pad", "mono −24 st ringmod drone"),
    126: ("Pad", "dark FM pad"),
    127: ("Pad", "adds a major 3rd, 3.5 s release"),
    128: ("Riser", "semitone flat, warbling; env2 opens filter over 3 s"),
    129: ("Pad Riser", "3 s swell, noise 30, closed filter"),
    130: ("Pad", "FM strings"),
    131: ("Pad", "string pad, 3 s attack"),
    132: ("Pad", "FM organ strings"),
    133: ("Pad", "strings, reverb 50"),
    134: ("Pad Lead", "string pad, 2.8 s release"),
    135: ("Pad", "unison pad, auto-pan, reverb 35"),
    136: ("Pad Chords", "phaser pad"),
    137: ("Pad Chords", "phaser pad, reverb"),
    138: ("Pad", "brass pad, 1.8 s attack"),
    139: ("Pad", "detuned saw pad, 1.1 s attack"),
    140: ("Pad", "unison swell, 2.4 s release"),
    141: ("Pad", "ringmod pad"),
    142: ("Pad", "phaser pad"),
    143: ("Pad", "noisy pad"),
    144: ("Chords Pad", "swept chord, 0.15 s attack"),
    145: ("Pad", "high ringmod pad"),
    146: ("–", "whole sound a fourth off"),
    147: ("Pad", "resonant pad, 3.7 s release"),
    148: ("Pad", "fifth pad: drone"),
    149: ("Pad", "fifth pad: drone, 3.5 s release"),
    150: ("Pad Chords", "chorused fifth pad"),
    151: ("Pad", "FM pad, 1.8 s attack"),
    152: ("Pad", "jet pad, 5.4 s release"),
    153: ("Pad", "FM pad, chorus"),
    154: ("–", "whole sound a fifth up"),
    155: ("Pad", "adds a major 3rd"),
    156: ("Pad", "adds a major 3rd"),
    157: ("Pad", "FM pad"),
    158: ("Bass", "synced LFO on the filter: wobble"),
    159: ("Bass", "bass under a saved arp"),
    160: ("Bass", "bass under a saved arp"),
    161: ("Bass", "FM bass under a saved arp"),
    162: ("Lead", "arp lead under a saved arp"),
    163: ("Lead", "arp lead under a saved arp"),
    164: ("–", "semitone sharp, saved arp"),
    165: ("Lead Bass", "acid once the saved arp is off: mono, −36 st, res 33"),
    166: ("Lead", "harpsichord under a saved arp"),
    167: ("Lead Bass", "metallic FM under a saved arp"),
    168: ("Lead", "sampled hang drum, 7 s tail"),
    169: ("Bass", "sampled double bass"),
    170: ("Lead", "sampled sitar, 8 s tail"),
}


def synth_table(card):
    d = os.path.join(card, "SYNTHS")
    rows = [
        synth_features(os.path.join(d, n))
        for n in sorted(os.listdir(d))
        if n.upper().endswith(".XML")
    ]
    return rows


def wav_seconds(path):
    try:
        with open(path, "rb") as fh:
            data = fh.read(1 << 16)
            size = os.path.getsize(path)
        if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
            return None
        pos, rate, block = 12, None, None
        while pos + 8 <= len(data):
            cid, clen = (
                data[pos : pos + 4],
                struct.unpack("<I", data[pos + 4 : pos + 8])[0],
            )
            if cid == b"fmt ":
                _ch, rate = struct.unpack("<HI", data[pos + 10 : pos + 16])
                block = struct.unpack("<H", data[pos + 20 : pos + 22])[0]
            if cid == b"data":
                clen = min(clen, size - pos - 8)
                return clen / (rate * block) if rate and block else None
            pos += 8 + clen + (clen & 1)
    except OSError:
        return None
    return None


def wav_shape(path, segments=8, stride=4):
    """Loudness (RMS, dBFS) and zero-crossing rate (a cheap brightness proxy, crossings per ms)
    for each of `segments` equal slices of the file. 16- and 24-bit PCM only; first channel."""
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        return None
    pos, fmt = 12, None
    while pos + 8 <= len(data):
        cid, clen = data[pos : pos + 4], struct.unpack("<I", data[pos + 4 : pos + 8])[0]
        if cid == b"fmt ":
            _tag, ch, rate = struct.unpack("<HHI", data[pos + 8 : pos + 16])
            bits = struct.unpack("<H", data[pos + 22 : pos + 24])[0]
            fmt = (ch, rate, bits)
        if cid == b"data" and fmt:
            ch, rate, bits = fmt
            body = data[pos + 8 : pos + 8 + clen]
            width = bits // 8
            if width not in (2, 3):
                return None
            frame = width * ch
            n = len(body) // frame
            vals = []
            for i in range(0, n, stride):
                o = i * frame
                if width == 2:
                    v = struct.unpack_from("<h", body, o)[0] / 32768
                else:
                    b = body[o : o + 3]
                    v = int.from_bytes(b, "little", signed=True) / 8388608
                vals.append(v)
            out = []
            seg = max(len(vals) // segments, 1)
            for k in range(segments):
                chunk = vals[k * seg : (k + 1) * seg]
                if not chunk:
                    break
                rms = (sum(x * x for x in chunk) / len(chunk)) ** 0.5
                zc = sum(1 for a, b in itertools.pairwise(chunk) if (a < 0) != (b < 0))
                ms = len(chunk) * stride / rate * 1000
                out.append((20 * math.log10(max(rms, 1e-6)), zc / ms))
            return out
        pos += 8 + clen + (clen & 1)
    return None


def audio_files(folder):
    return sorted(
        n
        for n in os.listdir(folder)
        if n.lower().endswith(AUDIO_EXT) and not n.startswith(".")
    )


def main():
    card, what = sys.argv[1], sys.argv[2]
    if what == "json":
        print(json.dumps(synth_table(card), indent=1))
    elif what == "synths":
        print(
            "| # | Preset | Engine | Voice | Uni | Pitch | Atk s | Sus | Rel s | Rule fit | Roles | Flags | Why |"
        )
        print("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
        for f in synth_table(card):
            num, name = f["file"][:3], f["file"][4:-4]
            eng = {"subtractive": "sub", "fm": "FM", "ringmod": "ring"}[f["mode"]]
            kn = key_note(f)
            fights = (
                fight_flags(f)
                + ([kn] if kn else [])
                + (["sample"] if f["samples"] else [])
            )
            roles, why = REVIEW[int(num)]
            print(
                f"| {num} | {name} | {eng} | {f['voice'].lower()} | {f['unison']} | {pitch_offset(f):+d} | "
                f"{f['atk_s']:.2f} | {f['sus']} | {f['rel_s']:.1f} | {' '.join(role_fit(f)) or '–'} | "
                f"{roles.replace(' ', ', ')} | {', '.join(fights)} | {why} |"
            )
    elif what == "counts":
        rows = synth_table(card)

        eng = collections.Counter(f["mode"] for f in rows)
        voice = collections.Counter(f["voice"] for f in rows)
        first = collections.Counter(
            REVIEW[int(f["file"][:3])][0].split()[0] for f in rows
        )
        anyr = collections.Counter(
            r for f in rows for r in REVIEW[int(f["file"][:3])][0].split()
        )
        flags = collections.Counter(
            fl.split()[0] for f in rows for fl in fight_flags(f)
        )
        keys = collections.Counter(key_note(f).split()[0] for f in rows if key_note(f))
        for role in ("Bass", "Chords", "Pad", "Lead"):
            cand = [f for f in rows if role in REVIEW[int(f["file"][:3])][0].split()]
            ok, arp_only, other = [], [], []
            for f in cand:
                fl = fight_flags(f)
                kn = key_note(f)
                bad = [
                    x
                    for x in fl
                    if x != "ARP" and not (role == "Pad" and x.startswith("rel"))
                ]
                if kn.startswith(("adds 3rd", "adds 2nd", "adds tritone")) and role in (
                    "Chords",
                    "Pad",
                ):
                    bad.append(kn)
                if role == "Chords" and kn.startswith("adds 5th"):
                    bad.append(kn)
                if bad:
                    other.append(f)
                elif "ARP" in fl:
                    arp_only.append(f)
                else:
                    ok.append(f)
            print(
                f"{role}: {len(cand)} listed, {len(ok)} clean, {len(arp_only)} clean once the arp is off, "
                f"{len(other)} with another flag"
            )
        fmf = [f for f in rows if f["mode"] == "fm"]
        print("engines", dict(eng))
        print("saved voice modes", dict(voice))
        print("best role", dict(first))
        print("any role", dict(anyr))
        print("flags", dict(flags))
        print("key notes", dict(keys))
        print(
            "FM presets",
            len(fmf),
            "with LPF < 50:",
            sum(1 for f in fmf if f["lpf"] < 50),
            "with filter cables:",
            sum(
                1
                for f in fmf
                if any(c[1].startswith(("lpf", "hpf")) for c in f["cables"])
            ),
            "lpfMode off:",
            sum(1 for f in fmf if f["lpf_mode"] == "off"),
        )
        print("saved arp on", [f["file"][:3] for f in rows if f["arp"]])
        print("portamento saved", [f["file"][:3] for f in rows if f["porta"]])
    elif what == "fmfilter":
        for f in synth_table(card):
            use = fm_filter_use(f)
            if f["mode"] == "fm":
                print(
                    f"| {f['file'][:3]} | {f['file'][4:-4]} | {f['lpf_mode']} | {', '.join(use) or '–'} |"
                )
    elif what == "drums":
        base = os.path.join(card, "SAMPLES", "DRUMS")
        for folder in sorted(os.listdir(base)):
            p = os.path.join(base, folder)
            if os.path.isdir(p):
                files = audio_files(p)
                durs = [wav_seconds(os.path.join(p, n)) for n in files]
                durs = [x for x in durs if x is not None]
                print(
                    f"| {folder} | {len(files)} | {min(durs):.2f}–{max(durs):.2f} | "
                    f"{', '.join(n.rsplit('.', 1)[0] for n in files)} |"
                )
    elif what == "fxshape":
        # Next arg: a text file listing card-relative sample paths, one per line.
        with open(sys.argv[3], encoding="utf-8") as fh:
            paths = fh.read().splitlines()
        for rel in paths:
            if not rel.strip():
                continue
            shape = wav_shape(os.path.join(card, rel))
            if shape is None:
                print(f"| {rel} | unreadable |")
                continue
            db = " ".join(f"{d:.0f}" for d, _ in shape)
            zc = " ".join(f"{z:.1f}" for _, z in shape)
            print(
                f"| {rel} | {wav_seconds(os.path.join(card, rel)):.1f} | {db} | {zc} |"
            )
    elif what == "samples":
        base = os.path.join(card, "SAMPLES")
        for dp, dn, fn in sorted(os.walk(base)):
            dn.sort()
            for n in sorted(fn):
                if n.lower().endswith(AUDIO_EXT):
                    p = os.path.join(dp, n)
                    sec = wav_seconds(p)
                    print(
                        f"{os.path.relpath(p, card)}\t{sec:.2f}"
                        if sec is not None
                        else f"{os.path.relpath(p, card)}\t?"
                    )


if __name__ == "__main__":
    main()
