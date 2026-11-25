"""Seam S6 in delugemu (spec §21.3, gismya/deluge-engine#5): a preset loaded and played over the test port.

    python3 scripts/generative/preset_emu.py [--elf ELF] [--out DIR] [--kit PATH [--drum NOTE]]

Boots the test-port build (build-testport/, built first unless --elf) under icount, as gen_emu.py capture does, on a
clone of the factory card with one pack/ synth (PRESET) put on it, and sends seam S6's lines: "hold midi_setup", then
"load synth PATH", then one note as timed "midi" lines on MIDI follow's channel A (channel 16: hold midi_setup).
It checks:
  - the lines the test port must refuse are refused (a parse error, nothing queued): bad "midi" bytes, a "load" with
    no path;
  - the load sends preset_loaded, and a kit that isn't on the card preset_load_failed why=missing;
  - the audio: silent in the second before the note, sounding while it is held.
--kit PATH also loads that kit, a file on the card (a factory kit: it is never committed), and checks one of its
drums (--drum, a MIDI note, default 36) sounds. Prints a PASS or FAIL line per check, then "N checks, M failed", and
exits 1 on a failure. Under icount the waits are the firmware's own virtual time, so the run repeats exactly; one
boot takes about a minute of wall time.

The Engine's Parity driver (deluge-engine E5) sends the same lines to the same build: load_and_play() is its
Reference step.
"""

# covers: src/deluge/io/midi/test_port.cpp src/deluge/io/midi/test_port.h

import argparse
import math
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import deluge_probe
import fat_image
import gen_emu
import test_port_emu

REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
PRESET = "SYNTHS/GENERATIVE/GEN Sine Sub.XML"  # a pack/ synth, no samples: its sound is the oscillator alone
NOTE = 0x3C  # C3 on the Deluge's display
FOLLOW_ON, FOLLOW_OFF = (
    0x9F,
    0x8F,
)  # MIDI follow's channel A under "hold midi_setup": channel 16
VELOCITY = 100
QUIET_MS = 1000  # before the note: no sound
HELD_MS = 1000
RELEASE_MS = 1500
LOAD_LIMIT_S = (
    30  # virtual seconds a load may take before the check gives up on its event
)
STEP_MS = 20  # virtual time between looks for an event
EDGE_S = 0.2  # left out at each end of a window: the event's step, and the audio's lag behind the virtual clock
LOUD_RMS = 10 * gen_emu.SILENCE_RMS  # a held note this loud at least

# Lines the test port must refuse with a parse error: nothing queued, nothing played.
REFUSED = (
    "midi f0",  # sysex start: no sysex over this line
    "midi f7",
    "midi 9f 3c",  # a note on takes two data bytes
    "midi 9f 3c 64 00",
    "midi 9f 3c 80",  # a data byte with its top bit set
    "midi 3c 64",  # no status byte
    "midi 9f 3c 1g",
    "midi 9f 3c 064",  # a byte is one or two hex digits
    "midi",
    "load synth",
    "load kit   ",
)


def card_with_preset(master, card):
    """Clones the factory card `master` to `card` and puts PRESET on the clone, from pack/."""
    gen_emu.clone_card(master, card)
    with open(os.path.join(REPO, "pack", *PRESET.split("/")), "rb") as fh:
        data = fh.read()
    with fat_image.FatImage(card) as image:
        image.write_file(PRESET, data)


def inject(emu, line):
    """Queues one line under icount: the VM runs in fixed steps until its reply (gen_emu.send_recipe())."""
    gen_emu.send_recipe(emu, [line])


def refused(emu, line):
    """Whether the test port refuses `line` with a parse error."""
    try:
        emu.port.inject_stepped(
            line,
            lambda: emu.wait(gen_emu.LINK_STEP_MS),
            gen_emu.REPLY_LIMIT_MS // gen_emu.LINK_STEP_MS,
        )
    except deluge_probe.InjectRejected as error:
        return error.status == deluge_probe.PARSE_ERROR
    return False


def wait_event(emu, prefix, seen, limit_s=LOAD_LIMIT_S):
    """The first event after the first `seen` that starts with `prefix`, without its timestamp: the VM runs in STEP_MS
    steps until one arrives. None after `limit_s` of virtual time."""
    end = emu.t + limit_s * gen_emu.SEC
    while True:
        emu.port.pump(0.01)
        for line in emu.port.events[seen:]:
            text = line.partition(": ")[2]
            if text.startswith(prefix):
                return text
        seen = len(emu.port.events)
        if emu.t >= end:
            gen_emu.log(f"no {prefix} event in {limit_s} s of virtual time")
            return None
        emu.wait(STEP_MS)


def load(emu, kind, path):
    """Sends "load KIND PATH" and returns its preset_loaded or preset_load_failed event, or None."""
    seen = len(
        emu.port.events
    )  # a short load sends its event before the inject's reply is read
    inject(emu, f"load {kind} {path}")
    return wait_event(emu, "preset_load", seen)


def note_lines(
    on, off, note, held_ms=HELD_MS, quiet_ms=QUIET_MS, release_ms=RELEASE_MS
):
    """One note as test-port lines: `quiet_ms` of nothing, the note on for `held_ms`, then `release_ms` after its off."""
    return [
        f"wait {quiet_ms}",
        f"midi {on:02x} {note:02x} {VELOCITY:02x}",
        f"wait {held_ms}",
        f"midi {off:02x} {note:02x} 40",
        f"wait {release_ms}",
    ]


def rms(pcm, start_s, end_s):
    """The left channel's RMS between `start_s` and `end_s` of S32LE stereo audio, of full scale."""
    a = int(start_s * gen_emu.RATE) * gen_emu.FRAME
    b = int(end_s * gen_emu.RATE) * gen_emu.FRAME
    chunk = pcm[a:b]
    left = struct.unpack(f"<{len(chunk) // 4}i", chunk)[0::2]
    if not left:
        return 0.0
    return math.sqrt(sum(v * v for v in left) / len(left)) / 2**31


def load_and_play(emu, kind, path, lines):
    """Loads the preset (load()), then queues `lines` (the notes, with their waits) and runs until they have played.
    Returns (the load's event, the audio from just before the first line went in). The lines are queued at once, one
    reply step apart: the firmware times them by its own waits."""
    event = load(emu, kind, path)
    if event is None or not event.startswith("preset_loaded "):
        return event, b""
    start = emu.dump_size()
    for line in lines:
        inject(emu, line)
    played_ms = sum(int(line.split()[1]) for line in lines if line.startswith("wait "))
    emu.wait(played_ms)
    gen_emu.run_for_audio(
        emu, start, played_ms / 1000
    )  # the audio lags the virtual clock a little
    return event, emu.audio(start, played_ms / 1000)


def run(args, elf):
    results = []

    def check(name, ok, detail=""):
        results.append(ok)
        gen_emu.log(
            f"{'PASS' if ok else 'FAIL'} {name}" + (f": {detail}" if detail else "")
        )

    os.makedirs(args.out, exist_ok=True)
    card = os.path.join(args.out, "preset-card.img")
    card_with_preset(args.card, card)
    try:
        emu = gen_emu.boot(args, elf, args.out, "preset", card=card, test_port=True)
        try:
            inject(emu, "hold midi_setup")
            for line in REFUSED:
                check(f"refused {line!r}", refused(emu, line))

            event, pcm = load_and_play(
                emu, "synth", PRESET, note_lines(FOLLOW_ON, FOLLOW_OFF, NOTE)
            )
            loaded = event is not None and event.startswith("preset_loaded type=synth ")
            check(
                "load synth",
                loaded and event.endswith(" path=" + PRESET),
                event or "no preset_load event",
            )
            if loaded:
                quiet = rms(pcm, EDGE_S, QUIET_MS / 1000 - EDGE_S)
                held = rms(
                    pcm, QUIET_MS / 1000 + EDGE_S, (QUIET_MS + HELD_MS) / 1000 - EDGE_S
                )
                check(
                    "silent before the note",
                    quiet < gen_emu.SILENCE_RMS,
                    f"rms {quiet:.2e}",
                )
                check("the held note sounds", held >= LOUD_RMS, f"rms {held:.2e}")
                if args.wav:
                    gen_emu.write_wav(os.path.join(args.out, "preset-synth.wav"), pcm)

            event = load(emu, "kit", "KITS/NO SUCH KIT.XML")
            check(
                "a missing kit",
                event
                == "preset_load_failed type=kit why=missing path=KITS/NO SUCH KIT.XML",
                event or "no preset_load event",
            )

            if args.kit:
                event, pcm = load_and_play(
                    emu, "kit", args.kit, note_lines(FOLLOW_ON, FOLLOW_OFF, args.drum)
                )
                check(
                    "load kit",
                    bool(event) and event.startswith("preset_loaded type=kit "),
                    event or "no event",
                )
                if pcm:
                    quiet = rms(pcm, EDGE_S, QUIET_MS / 1000 - EDGE_S)
                    hit = rms(pcm, QUIET_MS / 1000, (QUIET_MS + HELD_MS) / 1000)
                    check(
                        "silent before the drum",
                        quiet < gen_emu.SILENCE_RMS,
                        f"rms {quiet:.2e}",
                    )
                    check("the drum sounds", hit >= LOUD_RMS, f"rms {hit:.2e}")
            inject(emu, "release midi_setup")
        finally:
            emu.close()
    finally:
        for attempt in range(1, test_port_emu.BOOT_TRIES + 1):
            dump = os.path.join(args.out, gen_emu.boot_name("preset", attempt) + ".s32")
            if os.path.exists(dump):
                os.remove(dump)
        if os.path.exists(card):
            os.remove(card)
    failed = results.count(False)
    gen_emu.log(f"{len(results)} checks, {failed} failed")
    return 1 if failed else 0


def parser():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument(
        "--elf",
        help="this test-port firmware (ENABLE_SYSEX_TEST) instead of building build-testport/",
    )
    p.add_argument(
        "--out",
        default=os.path.join(REPO, "build", "preset_emu"),
        help="scratch folder: card, logs",
    )
    p.add_argument(
        "--kit", help="also load this kit, a path on the card, and play a drum"
    )
    p.add_argument(
        "--drum", type=lambda s: int(s, 0), default=36, help="--kit's note (default 36)"
    )
    p.add_argument(
        "--wav",
        action="store_true",
        help="keep the synth's audio as preset-synth.wav in --out",
    )
    p.add_argument(
        "--card",
        default=test_port_emu.SD_MASTER,
        help="the factory card image (never written)",
    )
    p.add_argument("--delugemu", default=test_port_emu.DELUGEMU)
    p.add_argument("--qemu", default=gen_emu.QEMU)
    return p


def main():
    args = parser().parse_args()
    args.out = os.path.abspath(args.out)  # run.sh runs in the delugemu checkout
    elf = args.elf or test_port_emu.build_testport()
    sys.exit(run(args, elf))


if __name__ == "__main__":
    main()
