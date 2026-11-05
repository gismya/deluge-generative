"""Build a small SD card folder holding one test song for delugemu captures.

The factory card has kits, synth presets and samples but no songs. This
script writes SONGS/CAPTURE.XML from three factory presets, and copies only
the samples those presets use, so the card image stays small and builds in
seconds:

  - kit   KITS/000 TR-808.XML          (sample drums)
  - synth SYNTHS/118 Small Bridge Pad  (subtractive, oscillators only)
  - synth SYNTHS/169 Double Bass       (subtractive, multisampled)

Usage: make_card.py <factory card root> <output folder>

The output folder is created (it must not exist). Factory content is copied,
never committed: the script reads it from the card the caller provides.
"""

import os
import re
import shutil
import sys

KIT = ("KITS", "000 TR-808")
PAD = ("SYNTHS", "118 Small Bridge Pad")
BASS = ("SYNTHS", "169 Double Bass")

# Ticks: 96 per quarter note, so 24 per 16th and 384 per 4/4 bar.
STEP = 24
BAR = 16 * STEP

# Kit rows: (drum index in the kit file, [16th steps], velocity).
# 000 TR-808 order: KICK SNARE HATC HATO MARACA RIM CLAV CLAP ...
KIT_ROWS = [
    (0, [0, 4, 8, 12], 90),  # KICK, four on the floor
    (1, [4, 12], 80),  # SNARE on 2 and 4
    (2, [2, 6, 10, 14], 90),  # HATC offbeats
    (3, [7, 15], 70),  # HATO
    (7, [12], 90),  # CLAP
]

# Pad: two bars, one chord per bar, held for most of the bar (MIDI notes).
PAD_CHORDS = [
    (0, [57, 60, 64]),  # A minor
    (BAR, [53, 57, 60]),  # F major
]
PAD_LEN = BAR - STEP

# Double bass: one bar, root notes on 8ths with a pickup.
BASS_NOTES = [(0, 33), (6, 33), (8, 45), (12, 33), (14, 36)]  # (16th step, note)
BASS_LEN = 2 * STEP


def note_hex(pos, length, velocity):
    # noteData: pos (8 hex), length (8 hex), velocity (2 hex), probability (2 hex).
    # Probability 0x14 is 20 out of 20, i.e. always plays.
    return f"{pos:08X}{length:08X}{velocity:02X}{0x14:02X}"


def note_data(notes):
    return "0x" + "".join(note_hex(p, l, v) for p, l, v in sorted(notes))


def read_preset(card, folder, name):
    path = os.path.join(card, folder, name + ".XML")
    with open(path, encoding="utf-8") as f:
        text = f.read()
    text = re.sub(r"<\?xml[^>]*\?>", "", text)
    # Preset files put version tags before the root element. A song carries
    # its own, so drop them.
    text = re.sub(r"<firmwareVersion>[^<]*</firmwareVersion>", "", text)
    text = re.sub(
        r"<earliestCompatibleFirmware>[^<]*</earliestCompatibleFirmware>", "", text
    )
    return text.strip()


def as_song_instrument(preset_xml, root, folder, name):
    """Give a preset's root element the attributes a song's <instruments> entry has."""
    opening = f"<{root}>"
    if not preset_xml.startswith(opening):
        raise SystemExit(f"unexpected preset root in {folder}/{name}")
    attrs = f'<{root} presetName="{name}" presetFolder="{folder}">'
    return attrs + preset_xml[len(opening) :]


PARAMS_RE = re.compile(r"<defaultParams>(.*?)</defaultParams>", re.DOTALL)


def split_params(preset_xml):
    """Pull every <defaultParams> block out of a preset.

    A preset keeps its params with the instrument, but a song keeps them in
    the clip (and, for a kit, in each drum's note row). Loading a song whose
    clip has no params freezes the firmware with E412. Returns the preset
    without its params, and the params' inner XML in file order: for a kit,
    the kit's own block first, then one per drum."""
    blocks = PARAMS_RE.findall(preset_xml)
    return PARAMS_RE.sub("", preset_xml), blocks


def clip_xml(preset, length, colour, params_tag, params, rows):
    folder, name = preset
    body = "\n".join(rows)
    return (
        "\t\t<instrumentClip\n"
        f'\t\t\tinstrumentPresetName="{name}"\n'
        f'\t\t\tinstrumentPresetFolder="{folder}"\n'
        '\t\t\tisPlaying="1"\n'
        f'\t\t\tlength="{length}"\n'
        '\t\t\tsection="0"\n'
        f'\t\t\tcolourOffset="{colour}">\n'
        f"\t\t\t<{params_tag}>{params}</{params_tag}>\n"
        "\t\t\t<noteRows>\n"
        f"{body}\n"
        "\t\t\t</noteRows>\n"
        "\t\t</instrumentClip>"
    )


def kit_clip(kit_params, drum_params):
    by_drum = {d: (steps, vel) for d, steps, vel in KIT_ROWS}
    rows = []
    # Every drum gets a row, so every drum has its params.
    for drum_index, params in enumerate(drum_params):
        steps, vel = by_drum.get(drum_index, ([], 0))
        nd = (
            f' noteData="{note_data([(s * STEP, STEP // 2, vel) for s in steps])}"'
            if steps
            else ""
        )
        rows.append(
            f'\t\t\t\t<noteRow drumIndex="{drum_index}"{nd}>\n'
            f"\t\t\t\t\t<soundParams>{params}</soundParams>\n"
            "\t\t\t\t</noteRow>"
        )
    return clip_xml(KIT, BAR, 0, "kitParams", kit_params, rows)


def synth_clip(preset, params, length, by_pitch, colour):
    rows = []
    for y in sorted(by_pitch):
        rows.append(f'\t\t\t\t<noteRow y="{y}" noteData="{note_data(by_pitch[y])}" />')
    return clip_xml(preset, length, colour, "soundParams", params, rows)


def pad_rows():
    by_pitch = {}
    for pos, notes in PAD_CHORDS:
        for n in notes:
            by_pitch.setdefault(n, []).append((pos, PAD_LEN, 80))
    return by_pitch


def bass_rows():
    by_pitch = {}
    for step, n in BASS_NOTES:
        by_pitch.setdefault(n, []).append((step * STEP, BASS_LEN, 60))
    return by_pitch


SONG_TEMPLATE = """<?xml version="1.0" encoding="UTF-8"?>
<song
\tfirmwareVersion="c1.3.0"
\tearliestCompatibleFirmware="4.1.0-alpha"
\txScroll="0"
\txZoom="24"
\tyScrollSongView="-7"
\ttimePerTimerTick="229"
\ttimerTickFraction="-1342177280"
\trootNote="9"
\tinputTickMagnitude="2"
\tswingAmount="0"
\tswingInterval="7"
\tsessionLayout="0">
\t<modeNotes>
\t\t<modeNote>0</modeNote>
\t\t<modeNote>2</modeNote>
\t\t<modeNote>3</modeNote>
\t\t<modeNote>5</modeNote>
\t\t<modeNote>7</modeNote>
\t\t<modeNote>8</modeNote>
\t\t<modeNote>10</modeNote>
\t</modeNotes>
\t<instruments>
%(instruments)s
\t</instruments>
\t<sections>
\t\t<section id="0" numRepeats="0" />
\t</sections>
\t<sessionClips>
%(clips)s
\t</sessionClips>
</song>
"""


def find_case_insensitive(card, rel):
    """FAT is case-insensitive and presets don't always match the on-disk case."""
    path = card
    for part in rel.split("/"):
        exact = os.path.join(path, part)
        if os.path.exists(exact):
            path = exact
            continue
        matches = [e for e in os.listdir(path) if e.lower() == part.lower()]
        if not matches:
            raise SystemExit(f"sample not found on card: {rel}")
        path = os.path.join(path, matches[0])
    return path


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    card, out = sys.argv[1], sys.argv[2]
    if os.path.exists(out):
        raise SystemExit(f"output folder exists: {out}")

    kit, kit_blocks = split_params(read_preset(card, *KIT))
    pad, (pad_params,) = split_params(read_preset(card, *PAD))
    bass, (bass_params,) = split_params(read_preset(card, *BASS))
    instruments = "\n".join(
        [
            as_song_instrument(kit, "kit", *KIT),
            as_song_instrument(pad, "sound", *PAD),
            as_song_instrument(bass, "sound", *BASS),
        ]
    )

    clips = "\n".join(
        [
            kit_clip(kit_blocks[0], kit_blocks[1:]),
            synth_clip(PAD, pad_params, 2 * BAR, pad_rows(), 20),
            synth_clip(BASS, bass_params, BAR, bass_rows(), 40),
        ]
    )
    song = SONG_TEMPLATE % {"instruments": instruments, "clips": clips}

    samples = sorted(set(re.findall(r"<fileName>([^<]+)</fileName>", instruments)))
    os.makedirs(os.path.join(out, "SONGS"))
    with open(os.path.join(out, "SONGS", "CAPTURE.XML"), "w", encoding="utf-8") as f:
        f.write(song)
    for rel in samples:
        src = find_case_insensitive(card, rel)
        dst = os.path.join(out, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(src, dst)
    print(
        f"wrote {os.path.join(out, 'SONGS', 'CAPTURE.XML')} with {len(samples)} samples"
    )


if __name__ == "__main__":
    main()
