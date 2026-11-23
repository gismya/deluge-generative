"""Generative mode in delugemu (spec §21.3, §21.5): smoke checks and the genre tuning capture.

    python3 scripts/generative/gen_emu.py smoke [--bars 8] [--no-leak]
    python3 scripts/generative/gen_emu.py leak [--cycles 2] [--cycle-script FILE]
    python3 scripts/generative/gen_emu.py golden [--testport-elf ELF] [--keep-card]
    python3 scripts/generative/gen_emu.py capture --genre techno-acid --seed 7 --energy 40 --seconds 60
    python3 scripts/generative/gen_emu.py capture --genre techno-dub --seconds 120 --mute 3 --ab --no-play
    python3 scripts/generative/gen_emu.py check WAV... --genre techno-dub --seconds 120 --mute 3

smoke    boots a local `./dbt build release` (build/Release/deluge.elf) on a per-run clone of the card image, retrying a
         blank boot, enters Perform through SHIFT+SONG -> GENERATIVE -> PERFORM FROM -> GENRE -> TECHNO -> the default
         genre -> DISCARD SONG? -> OK, presses PLAY and plays at least N bars (2 s a bar, so N bars at any tempo from
         120 BPM). It fails on silence (any half second of digital silence in the firmware's rendered audio) or a freeze
         (the OLED, which shows the beat, not changing for a second), checks that PLAY stops, and leaves with Capture
         (BACK held -> LEAVE, KEEP: LAST). It then reloads the saved Capture song through LOAD, checks it plays and
         sounds, and reads the new song file off the card clone (golden_song.capture_song_summary()) to confirm it has
         valid clipInstances and notes. Functional only: no audio hashes. Then runs the leak check below (skip it with
         --no-leak) and reports both: one failing doesn't hide the other.

leak     the memory check (spec §13.8: memory after leaving matches memory before entry). Builds the test-port
         firmware (build-testport/, Release with -DENABLE_SYSEX_TEST=ON; build/ keeps the default options), boots it
         and runs perform_mem_cycle.txt (or --cycle-script FILE, e.g. a re-roll cycle: it must enter and leave
         Perform once) --cycles times on one boot over the test port, then reads the gen_mem
         before_entry / after_leave events. Fails unless, for every cycle after the first WARMUP_CYCLES, internal and
         external free memory after leaving equal their values before entry exactly.

golden   the firmware check and the Compose and Recipe-file smoke checks (spec §21.3). On the test-port firmware
         (build-testport/, built first unless --testport-elf), runs golden_compose.txt: under "hold golden" the Compose
         screen rolls a genre's golden case (its default Recipe at the goldens' seed, takes 0), SAVE writes its Recipe
         file and "check recipe *" reads it back, BUILD composes it from the Groove as the golden plays from
         SHIFT+PLAY, and the song is saved as SONGS/GOLDEN.XML and loaded back. Then reads the song and the new Recipe
         file off the card clone (golden_song.py) and checks the Recipe file is the golden's case and the song's notes
         are the golden's (tests/spec/generator/goldens/<genre>.txt, the genre the script picks): both as the
         golden's note lines, the notes that start more than kMaxLean before the golden's last tick, cut there. A
         difference prints as a diff; both dumps are left in --out (golden-notes.txt, song-notes.txt).
capture  the tuning loop's main step: builds a capture firmware in build-capture/ (--build-dir; --elf: that one, built
         before, and nothing builds), writes a copy of its ELF beside the WAV whose Current Recipe and seed come from
         the arguments (genre, Recipe overrides, seed; the same flags as the harness's generator_render), boots it,
         plays Perform for --seconds and writes the rendered audio to a WAV, then plays it (afplay) unless
         --no-play. Deterministic: the same arguments give the same WAV. The Recipe isn't in the build, so a new
         Recipe rebuilds nothing; a source change rebuilds and relinks. --out is a folder (the WAV is named after
         the arguments, never overwriting one) or a .wav path.
         --mute PART (sidebar rows 1-6, repeatable) mutes a part from the first step: SHIFT+its mute pad right after
         PLAY, so the piece plays as without the mute and only that part is silent (no genre data patched).
         --end-after-drop [N] (default 1; put WAV paths before it, or give N, or it reads the WAV as N) or
         --end-at-bar BAR presses End: it holds the End pad (AUDITION/SECTION
         row 8) from the Nth Drop's first bar line, or bar BAR's (bar 0 at PLAY), so End arms in that bar and the
         Outro follows from the next phrase line; the transport stops where it ends and the rest of --seconds is
         silence. The Drop's bar comes from the harness render, which arms End the same way (generator_render
         --end-after-drop N, --end-at-bar BAR), so the check reads the ended piece. With --no-check it still needs
         generator_render (--render, or the newest under build/tests/). The log says where the piece ends.
         --ab with --mute: two captures, without and with the mutes, then their levels and the muted parts' estimated
         level (wav_levels.py --full). Leave the first second out of a level of your own (--skip 1): the mute lands
         on the first step.
         Each capture keeps its patched ELF as <WAV stem>-pid<N>.elf and its raw dump and run log as
         <WAV stem>-pid<N>-boot<attempt>.s32/.log beside the WAV, so captures sharing a --out folder or a --build-dir
         never patch the same firmware or read each other's audio. The emulator's audio runs a little behind its
         virtual clock (13-60 ms over a few minutes), so a capture runs on past --seconds until the dump holds the
         whole --seconds (at most TAIL_LIMIT_S more): the WAV is exactly --seconds long. The ELF and dumps are deleted after, the logs when
         the audio is whole; a capture also deletes the ELFs, dumps (and the empty WAV) of captures that were killed. It
         then runs the check below on its WAV and exits 1 if the audio isn't whole: after the firmware it builds
         generator_render (only that target, in build/tests/) and renders the same Recipe, so the check reads what
         the firmware plays (--render: that one instead). --no-check skips it: the length and a corrupt run still
         fail, and silence is a WARNING.

check    is a capture whole? Takes the WAVs and the arguments they were captured with (Recipe, --seconds, --mute).
         Fails a WAV that is shorter than --seconds, silent for the beat after a 16th where the harness render of the
         same Recipe (generator_render --midi-only) starts a note in a part the capture didn't mute (step 0 at PLAY;
         the last beat is cut at --seconds; silence elsewhere is fine, so isolated captures and sparse sections
         pass), or is at full scale for 50 ms in a row (clipped flat or corrupt; a shorter run is a WARNING: the
         mix clips). A capture made with --end-after-drop or --end-at-bar takes the same flag, so the render ends
         as it did. Exit 1 if any WAV fails. Reads the newest generator_render under build/tests/ (or --render)
         and warns if the Generator's source changed after it was built.

Everything runs under icount (-icount shift=2,sleep=off -S): the VM only moves in run_until steps, every input goes in
at a fixed virtual time through button() and every wait counts virtual time. pylib's wall-clock helpers (press,
load_song, wait_until, wait_sd_idle) are never used.

The delugemu the checks last passed with is pinned in delugemu.pin beside this script. A run warns when the installed
bundle's VERSION or the delugemu checkout (which provides pylib and run.sh) differs. `smoke --update-pin` records
what is installed once the checks pass on it; commit that on purpose.

Needs a delugemu source checkout (~/source/delugemu), its nightly bundle's QEMU and the factory card image
(~/source/delugemu-sd/deluge_sd.img, attached with -snapshot so it is never written).
"""

import argparse
import array
import collections
import contextlib
import difflib
import fcntl
import glob
import hashlib
import itertools
import math
import os
import re
import shlex
import struct
import subprocess
import sys
import tempfile
import time
import wave
import xml.etree.ElementTree as ET

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import deluge_probe
import fat_image
import golden_song
import test_port_emu
import theory_review

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
HOME = os.path.expanduser("~")
PIN_PATH = os.path.join(HERE, "delugemu.pin")
QEMU = os.path.join(
    HOME,
    "source",
    "delugemu-nightly",
    "DelugEmu-macos-arm64",
    "DelugEmu.app",
    "Contents",
    "MacOS",
    "qemu-system-arm",
)

RATE = 44100
FRAME = 8  # S32LE stereo, as DELUGEMU_SSIF_DUMP writes it
MS = 1_000_000
SEC = 1_000_000_000
STEP = 100 * MS  # how far the VM runs between looks at it
SECONDS_PER_BAR = 2.0  # 4 beats at 120 BPM, the slowest tempo a genre rolls
SILENCE_RMS = 1e-4  # of full scale; the firmware's idle output is exact zeros
SILENCE_WINDOW = 0.5  # s: a 4/4 genre at 120 BPM or faster has a beat in every one
FREEZE_WINDOW = (
    SEC  # the Perform OLED shows the beat, so it changes at least twice a second
)
TICKS_PER_STEP = theory_review.TICKS_PER_STEP  # the harness MIDI's 16th, at 96 PPQN
STEPS_PER_BAR = theory_review.STEPS_PER_BAR
RISER_LANE = (
    "Riser"  # the harness MIDI's lane name (tests/spec/generator/render/names.cpp)
)
# The End pad: AUDITION/SECTION (x 17), row 8 (firmware y 0). End arms after a 600 ms hold (spec §13.5).
END_PAD = (17, 0)
END_HOLD_MS = 800
# How far past --seconds a capture may run for the audio its dump lags behind the virtual clock: a few blocks.
TAIL_LIMIT_S = 1.0
WINDOW_STEPS = 4  # a beat: a sounding step must have sound within it, so firmware latency can't fail a capture
MAX_BARS = 10000  # the most generator_render --bars takes
MAX_TEMPO = 999  # the fastest generator_render --tempo takes
FULL_SCALE = 2**31 - 256  # a sample this close to full scale
# The firmware saturates its mix at the int32 limits, so a loud kick can hold full scale for a few hundred samples:
# a run that long is a WARNING (the mix clips). A run longer than half a cycle of 10 Hz is no audio at all: a FAIL.
FULL_SCALE_RUN = 64  # samples in a row (1.5 ms) at full scale: clipped
CORRUPT_RUN = (
    RATE // 20
)  # 50 ms in a row at full scale: clipped flat for longer than any audio, or corrupt

# The leak check (spec §13.8).
LEAK_CYCLES = (
    2  # one warm-up and one checked: #167 leaked about 1 KB every cycle (#314)
)
# The first cycle enters from the boot song, whose teardown and first loads may warm caches; perform_runner.txt has
# always called the second gen_mem pair the one that must match. Cycles after it must return both heaps exactly. A
# change of a few bytes either way is still a finding: the heap layout repeats exactly when everything is freed, so a
# moved block means a held one.
WARMUP_CYCLES = 1
GEN_MEM = re.compile(
    r"gen_mem (before_entry|after_leave) internal=(\d+) external=(\d+)"
)

KEYS = {"C": 0, "C#": 1, "Db": 1, "D": 2, "D#": 3, "Eb": 3, "E": 4, "F": 5, "F#": 6, "Gb": 6, "G": 7, "G#": 8,
        "Ab": 8, "A": 9, "A#": 10, "Bb": 10, "B": 11}  # fmt: skip
MACROS = ("energy", "density", "complexity", "motion")


def log(msg):
    print("[gen-emu] " + msg, flush=True)


# -- checks ------------------------------------------------------------------


def audio_failures(pcm, seconds, window=SILENCE_WINDOW):
    """Why `seconds` of rendered S32LE stereo audio isn't a playing Perform: too short, or silent somewhere."""
    failures = []
    frames = len(pcm) // FRAME
    have = frames / RATE
    if have + 0.01 < seconds:
        failures.append(f"only {have:.1f} s of audio rendered, wanted {seconds:.1f} s")
    span = int(window * RATE)
    silent_from = None
    start = 0
    while (
        start + span <= min(frames, int(seconds * RATE) + span - 1) and start < frames
    ):
        chunk = pcm[start * FRAME : min(start + span, frames) * FRAME]
        left = struct.unpack(f"<{len(chunk) // 4}i", chunk)[0::2]
        rms = math.sqrt(sum(v * v for v in left[::4]) / max(1, len(left[::4]))) / 2**31
        if rms < SILENCE_RMS and silent_from is None:
            silent_from = start
        elif rms >= SILENCE_RMS and silent_from is not None:
            failures.append(
                f"silent from {silent_from / RATE:.1f} s to {start / RATE:.1f} s"
            )
            silent_from = None
        start += span
    if silent_from is not None:
        failures.append(
            f"silent from {silent_from / RATE:.1f} s to {min(start, frames) / RATE:.1f} s"
        )
    return failures


def longest_full_scale_run(samples):
    """(length, seconds in) of the longest run of full-scale samples at one limit in one channel of interleaved stereo
    `samples`, or None if no sample is at full scale. A swing from one limit to the other starts a new run."""
    best = None
    for channel in (0, 1):
        run = 0
        sign = 0
        for i, v in enumerate(samples[channel::2]):
            at = 1 if v >= FULL_SCALE else -1 if v <= -FULL_SCALE else 0
            if at and at == sign:
                run += 1
                continue
            if run and (best is None or run > best[0]):
                best = run, (i - run) / RATE
            run, sign = (1, at) if at else (0, 0)
        if run and (best is None or run > best[0]):
            best = run, (len(samples) // 2 - run) / RATE
    return best


def bar_runs(bars, bar_seconds):
    """`1-2 at 0.5 s, 7 at 3.5 s` for sorted bars."""
    runs = []
    for b in bars:
        if runs and runs[-1][1] == b - 1:
            runs[-1][1] = b
        else:
            runs.append([b, b])
    return ", ".join(
        (f"{a}" if a == b else f"{a}-{b}") + f" at {a * bar_seconds:g} s"
        for a, b in runs
    )


WavVerdict = collections.namedtuple("WavVerdict", "failures warnings checked")


def wav_failures(pcm, seconds, steps, step_seconds, quiet_bars=frozenset()):
    """Whether S32LE stereo audio is a whole capture of `seconds`, as a WavVerdict:
    - failures: short; silent in the beat after a step of `steps` (the onsets of the harness render's notes in parts
      the capture didn't mute; step 0 starts at PLAY), with the last beats clipped to the capture's end; or a run of
      CORRUPT_RUN full-scale samples. Silence anywhere else is fine: an isolated or sparse capture is silent where its
      parts rest, and so are the bars in `quiet_bars` (--quiet-bars: a riser alone starts from silence).
      With `steps` given but none of them checked (an FX-alone capture before its first note), audio that is silent
      throughout fails too.
    - warnings: a run of FULL_SCALE_RUN full-scale samples (the mix clips); `steps` given but none checked, with
      some sound (naming `quiet_bars` when they held every step in it).
    - checked: how many steps' beats it checked.
    `steps` None: no harness render, so no step is checked."""
    failures, warnings = [], []
    frames = len(pcm) // FRAME
    if frames / RATE + 0.01 < seconds:
        failures.append(f"{frames / RATE:.1f} s of audio, wanted {seconds:.1f} s")
    samples = array.array("i", pcm[: frames * FRAME])
    limit = min(frames, int(seconds * RATE))
    step_frames = step_seconds * RATE
    silent_bars = set()
    checked = quiet = 0
    for step in sorted(steps or ()):
        start = int(step * step_frames)
        if start + step_frames > limit:
            continue  # less than a 16th before the capture's end (or where it stopped short): latency would fail it
        if step // STEPS_PER_BAR in quiet_bars:
            quiet += 1
            continue
        end = min(limit, int((step + WINDOW_STEPS) * step_frames))
        chunk = samples[start * 2 : end * 2 : 11]
        rms = math.sqrt(sum(v * v for v in chunk) / max(1, len(chunk))) / 2**31
        checked += 1
        if rms < SILENCE_RMS:
            silent_bars.add(step // STEPS_PER_BAR)
    if silent_bars:
        failures.append(
            f"silent in {len(silent_bars)} bar(s) where the harness has notes (bar 0 at PLAY): "
            + bar_runs(sorted(silent_bars), step_seconds * STEPS_PER_BAR)
        )
    if steps is not None and not checked:
        chunk = samples[: limit * 2 : 11]
        # every note the harness has in it is in --quiet-bars: say so, it may be the bars named, not the music
        why = f"the harness's {quiet} steps in it are all in quiet bars"
        if not any(abs(v) / 2**31 >= SILENCE_RMS for v in chunk):
            failures.append(
                f"silent throughout, and --quiet-bars left no step to check ({why})"
                if quiet
                else "silent throughout, and the harness has no note to check in it"
            )
        else:
            warnings.append(
                f"--quiet-bars left no step to check: {why}; only its length and full-scale runs were checked"
                if quiet
                else "the harness has no note to check in it: only its length and full-scale runs were checked"
            )
    run = longest_full_scale_run(samples)
    if run and run[0] >= CORRUPT_RUN:
        failures.append(
            f"a run of {run[0]} full-scale samples at {run[1]:.1f} s (clipped or corrupt)"
        )
    elif run and run[0] >= FULL_SCALE_RUN:
        warnings.append(
            f"a run of {run[0]} full-scale samples at {run[1]:.1f} s: the mix clips"
        )
    return WavVerdict(failures, warnings, checked)


def bar_set(text):
    """--quiet-bars' value: bars from PLAY (bar 0), comma-separated, each a bar or a range: `47` or `3,40-42`."""
    bars = set()
    for item in text.split(","):
        first, dash, last = item.strip().partition("-")
        try:
            a, b = int(first), int(last if dash else first)
        except ValueError:
            raise argparse.ArgumentTypeError(
                f"{text!r}: bars from 0, as 47 or 3,40-42"
            ) from None
        if a < 0 or b < a:
            raise argparse.ArgumentTypeError(f"{text!r}: bars from 0, as 47 or 3,40-42")
        bars.update(range(a, b + 1))
    return frozenset(bars)


def drop_count(text):
    """--end-after-drop's N: 1 to 99, as generator_render takes it."""
    try:
        n = int(text)
    except ValueError:
        n = 0
    if not 1 <= n <= 99:
        raise argparse.ArgumentTypeError("takes a Drop count, 1 to 99")
    return n


def bar_number(text):
    """--end-at-bar's BAR: 0 to 9999, as generator_render takes it."""
    try:
        n = int(text)
    except ValueError:
        n = -1
    if not 0 <= n <= 9999:
        raise argparse.ArgumentTypeError("takes a bar, 0 to 9999")
    return n


def quiet_bars(args):
    """Every bar the --quiet-bars flags name."""
    return frozenset().union(*(getattr(args, "quiet_bars", None) or []))


def active_steps(parts, muted):
    """The 16th steps where a note starts in a part not in `muted` (sidebar rows, 1-based: the genre's part order).
    Onsets only: a held note's sound may decay before its note-off. A Riser lane's note counts on the beat before its
    note-off, the Drop, not on its onset: the firmware starts a sample Riser late so it ends on the Drop
    (generative/perform/riser_timing.h, spec §9.5), from the length of the sample it picks off the card, which the
    render doesn't know. A synth Riser's note counts there too: the render doesn't say which kind a lane plays, and a
    synth Riser sounds to the Drop as well. `parts` are theory_review Parts, in the genre's order."""
    steps = {
        riser_step(n)
        if part.lane_notes.get(n.key) == RISER_LANE
        else n.tick // TICKS_PER_STEP
        for row, part in enumerate(parts, start=1)
        if row not in muted
        for n in part.notes
    }
    steps.discard(None)
    return steps


def riser_step(note):
    """The step a Riser's note is checked on: the beat before its note-off (the Drop, or the render's end, past the
    capture's), or its onset if that's later. None for a Riser whose note-off never comes: its Drop is past the render."""
    if note.end is None:
        return None
    return max(note.tick // TICKS_PER_STEP, note.end // TICKS_PER_STEP - WINDOW_STEPS)


def freeze_failures(samples, window_ns=FREEZE_WINDOW):
    """Why OLED samples [(vclock_ns, frame)] aren't a live Perform: the frame stopped changing for `window_ns`."""
    if not samples:
        return ["no OLED samples"]
    t0 = samples[0][0]
    changed_at, last = t0, samples[0][1]
    for t, frame in samples[1:]:
        if frame != last:
            changed_at, last = t, frame
        elif t - changed_at >= window_ns:
            return [
                "the OLED stopped changing at %.1f s (frozen?)"
                % ((changed_at - t0) / SEC)
            ]
    return []


def mem_cycles(events):
    """Each completed enter/leave cycle's free memory, from gen_mem test events: [(before, after)], each a
    {"internal": B, "external": B} dict. An entry with no after_leave (cancelled, or failed) pairs with nothing."""
    cycles, before = [], None
    for line in events:
        m = GEN_MEM.search(line)
        if not m:
            continue
        sample = {"internal": int(m.group(2)), "external": int(m.group(3))}
        if m.group(1) == "before_entry":
            before = sample
        elif before is not None:
            cycles.append((before, sample))
            before = None
    return cycles


def leak_report(cycles, wanted, warmup=WARMUP_CYCLES):
    """(lines, failures) for the leak check: a line of deltas per cycle, and why it fails, if it does."""
    lines, failures = [], []
    for n, (before, after) in enumerate(cycles, 1):
        d_int = after["internal"] - before["internal"]
        d_ext = after["external"] - before["external"]
        checked = n > warmup
        lines.append(
            f"cycle {n}: internal {before['internal']} -> {after['internal']} ({d_int:+d} B), "
            f"external {before['external']} -> {after['external']} ({d_ext:+d} B)"
            + ("" if checked else " [warm-up, not checked]")
        )
        if not checked:
            continue
        for heap, delta in (("internal", d_int), ("external", d_ext)):
            if delta != 0:
                failures.append(
                    f"cycle {n}: {heap} free memory changed by {delta:+d} B across enter/leave; must be 0"
                )
    if len(cycles) < wanted:
        failures.append(
            f"only {len(cycles)} of {wanted} enter/leave cycles completed (see the script output)"
        )
    if len(cycles) <= warmup:
        failures.append(f"no cycle after the {warmup} warm-up cycle(s) to check")
    return lines, failures


# -- the delugemu pin --------------------------------------------------------


def read_pin(path=PIN_PATH):
    """The pinned delugemu, {} if nothing is pinned yet."""
    pin = {}
    if not os.path.exists(path):
        return pin
    with open(path) as fh:
        for line in fh:
            line = line.strip()
            if line and not line.startswith("#"):
                key, _, value = line.partition("=")
                pin[key.strip()] = value.strip()
    return pin


def write_pin(installed, path=PIN_PATH):
    with open(path, "w") as fh:
        fh.write(
            "# The delugemu the Generative checks last passed with (spec §21.3). gen_emu.py warns when the installed\n"
            "# one differs. Bump it on purpose: gen_emu.py smoke --update-pin, then commit.\n"
            "bundle={}\ncheckout={}\n".format(
                installed["bundle"], installed["checkout"]
            )
        )


def installed_versions(qemu, delugemu):
    version = os.path.join(
        os.path.dirname(os.path.dirname(qemu)), "Resources", "VERSION"
    )
    bundle = "unknown"
    if os.path.exists(version):
        with open(version) as fh:
            bundle = fh.read().strip()
    checkout = subprocess.run(
        ["git", "-C", delugemu, "rev-parse", "HEAD"],
        capture_output=True,
        text=True,
        check=False,
    ).stdout.strip()
    return {"bundle": bundle, "checkout": checkout or "unknown"}


def pin_warnings(pin, installed):
    if not pin:
        return [
            "no delugemu is pinned yet: run gen_emu.py smoke --update-pin once the checks pass"
        ]
    warnings = []
    if installed.get("bundle") != pin.get("bundle"):
        warnings.append(
            "the installed delugemu bundle is {}, but the checks last passed with {} (delugemu.pin)".format(
                installed.get("bundle"), pin.get("bundle")
            )
        )
    if installed.get("checkout") != pin.get("checkout"):
        warnings.append(
            "the delugemu checkout (pylib, run.sh) is at {}, but the checks last passed with {} (delugemu.pin)".format(
                installed.get("checkout"), pin.get("checkout")
            )
        )
    return warnings


# -- the capture build -------------------------------------------------------


def genre_names():
    """Each genre's stable name, from the genre tables."""
    folder = os.path.join(REPO, "src", "deluge", "generative", "core", "genres")
    names = []
    for name in sorted(os.listdir(folder)):
        if name.endswith(".cpp"):
            with open(os.path.join(folder, name)) as fh:
                text = fh.read()
                # A genre written whole, unless it is a parent (version 0: never played) ...
                for body in re.findall(
                    r"(?:const|constexpr) Genre\s+\w+\s*\{(.*?)\n\};", text, re.DOTALL
                ):
                    if not re.search(r"\.version\s*=\s*0\b", body):
                        names += re.findall(
                            r"^\s*\.name\s*=\s*\"([^\"]+)\"", body, re.MULTILINE
                        )[:1]
                # ... or a subgenre, written as overrides of its parent (spec §4.1).
                names += re.findall(
                    r"subgenreOf\(\w+\);\s*genre\.name\s*=\s*\"([^\"]+)\"", text
                )
    return names


def scale_names():
    """Scale values in order, as kebab-case names ("harmonic-minor"), from the core's Scale enum."""
    with open(
        os.path.join(REPO, "src", "deluge", "generative", "core", "types.h")
    ) as fh:
        body = re.search(
            r"enum class Scale : uint8_t \{(.*?)\};", fh.read(), re.DOTALL
        ).group(1)
    names = [n.strip() for n in body.split(",") if n.strip()]
    return [re.sub(r"(?<!^)(?=[A-Z])", "-", n).lower() for n in names]


# The capture firmware's CaptureRecipe (generative/firmware/current_recipe.cpp): the Recipe it plays, which
# write_capture_elf patches into a copy of its ELF. The two layouts must agree; the firmware static_asserts its own.
CAPTURE_SYMBOL = b"generative_capture_recipe"
CAPTURE_MAGIC = b"GENCAP1\0"
CAPTURE_LAYOUT = struct.Struct(
    "<8s32sIIi4bbbbx"
)  # magic, genre, entropy, seed, tempo, macros, key, scale, swing
CAPTURE_GENRE_BYTES = 32
KEEP_SWING = -128  # RecipeOverrides::kKeepSwing


def capture_recipe(args):
    """The CaptureRecipe bytes for these arguments: a RecipeOverrides (generative/core/recipe_overrides.h) with the
    genre's name inline. Exits on an unknown genre, key or scale."""
    if args.genre not in genre_names():
        sys.exit(
            "unknown genre: {} (have {})".format(args.genre, ", ".join(genre_names()))
        )
    key = -1
    if args.key is not None:
        if args.key not in KEYS:
            sys.exit(f"unknown key: {args.key}")
        key = KEYS[args.key]
    scale = -1
    if args.scale is not None:
        if args.scale not in scale_names():
            sys.exit(
                "unknown scale: {} (have {})".format(
                    args.scale, ", ".join(scale_names())
                )
            )
        scale = scale_names().index(args.scale)
    genre = args.genre.encode()
    if len(genre) >= CAPTURE_GENRE_BYTES:  # the firmware needs its terminating NUL
        sys.exit(f"genre name too long for a capture: {args.genre}")
    return CAPTURE_LAYOUT.pack(
        CAPTURE_MAGIC,
        genre,
        args.entropy,
        args.seed,
        0 if args.tempo is None else hundredths(args.tempo),
        *(-1 if getattr(args, m) is None else getattr(args, m) for m in MACROS),
        key,
        scale,
        KEEP_SWING if args.swing is None else args.swing,
    )


def capture_recipe_offset(elf, path):
    """The file offset of the CaptureRecipe in `elf` (an ELF32 file's bytes). Exits if it has none."""
    if elf[:6] != b"\x7fELF\x01\x01":
        sys.exit(f"{path}: not a little-endian 32-bit ELF")
    shoff, shentsize, shnum = (
        struct.unpack_from("<I", elf, 32)[0],
        *struct.unpack_from("<HH", elf, 46),
    )
    sections = [
        struct.unpack_from("<10I", elf, shoff + i * shentsize) for i in range(shnum)
    ]  # name, type, flags, addr, offset, size, link, info, addralign, entsize
    for _, kind, _, _, offset, size, link, _, _, entsize in sections:
        if kind != 2:  # SHT_SYMTAB
            continue
        strtab = sections[link][4]
        for at in range(offset, offset + size, entsize):
            name, value, sym_size, _, _, shndx = struct.unpack_from("<IIIBBH", elf, at)
            end = elf.index(b"\0", strtab + name)
            if elf[strtab + name : end] != CAPTURE_SYMBOL:
                continue
            if shndx >= len(
                sections
            ):  # SHN_ABS, SHN_COMMON or out of range: not a capture build's
                break
            home = sections[shndx]
            if home[1] != 1 or sym_size != CAPTURE_LAYOUT.size:  # SHT_PROGBITS
                break
            start = home[4] + value - home[3]
            if elf[start : start + len(CAPTURE_MAGIC)] != CAPTURE_MAGIC:
                break
            return start
    sys.exit(
        f"{path} has no {CAPTURE_LAYOUT.size}-byte {CAPTURE_SYMBOL.decode()} of this gen_emu's layout: not a capture "
        "build (gen_emu.py capture builds one in --build-dir), or one from another gen_emu"
    )


def write_capture_elf(src, dst, recipe):
    """Writes a copy of the capture firmware `src` to `dst` that plays `recipe` (capture_recipe's bytes)."""
    with open(src, "rb") as fh:
        elf = bytearray(fh.read())
    start = capture_recipe_offset(elf, src)
    elf[start : start + len(recipe)] = recipe
    with open(dst, "wb") as fh:
        fh.write(elf)


def uint32(text):
    """A seed or entropy, as generator_render takes it: 0 <= n < 2^32."""
    n = int(text)  # base 10, as generator_render reads it
    if not 0 <= n < 2**32:
        raise argparse.ArgumentTypeError("takes a 32-bit number")
    return n


def bpm(text):
    """A tempo, as generator_render takes it: 20 to 999 BPM (NaN refused)."""
    value = float(text)
    if not (value >= 20 and value <= MAX_TEMPO):
        raise argparse.ArgumentTypeError("takes 20 to 999 BPM")
    return value


def hundredths(tempo):
    """Hundredths of a BPM, rounded as generator_render rounds them."""
    return int(tempo * 100 + 0.5)


def is_wav_path(out):
    """Whether --out names a WAV file rather than a folder."""
    return out.lower().endswith(".wav")


def capture_path(args, out, mutes=()):
    """`out` itself if it names a .wav file (overwritten, and not claimed: give captures that may run at once a
    folder); else out/<every argument, and the parts in `mutes`>.wav, with -2, -3 ... added rather than overwrite an
    earlier capture."""
    if is_wav_path(out):
        return out
    name = f"{args.genre}-seed{args.seed}"
    if args.entropy:
        name += f"-entropy{args.entropy}"
    for m in MACROS:
        if getattr(args, m) is not None:
            name += f"-{m}{getattr(args, m)}"
    if args.key is not None:
        name += f"-key{args.key}"
    if args.scale is not None:
        name += f"-{args.scale}"
    if args.tempo is not None:
        name += f"-tempo{args.tempo:g}"
    if args.swing is not None:
        name += f"-swing{args.swing}"
    for part in sorted(set(mutes)):
        name += f"-mute{part}"
    if getattr(args, "end_after_drop", None):
        name += f"-end-drop{args.end_after_drop}"
    elif getattr(args, "end_at_bar", None) is not None:
        name += f"-end-bar{args.end_at_bar}"
    name += f"-{args.seconds:g}s"
    n = 1
    while True:
        path = os.path.join(out, name + ("" if n == 1 else f"-{n}") + ".wav")
        try:  # claims the name now, so a capture running beside this one can't pick it too
            os.close(os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644))
            return path
        except FileExistsError:
            n += 1


def capture_tag(wav):
    """The name of a capture's raw dumps and run logs beside its WAV: the WAV's stem and this process, so captures
    sharing a folder never read each other's audio."""
    return f"{os.path.splitext(os.path.basename(wav))[0]}-pid{os.getpid()}"


# A boot attempt's raw dump (.s32) and run log (.log): <capture_tag>-boot<attempt>.<ext>.
BOOT_FILE = re.compile(
    r"^(?P<tag>(?P<stem>.+)-pid(?P<pid>\d+))-boot(?P<attempt>\d+)\.(?P<ext>s32|log)$"
)


# A capture's copy of the firmware, patched with its Recipe: <capture_tag>.elf.
CAPTURE_ELF = re.compile(r"^(?P<stem>.+)-pid(?P<pid>\d+)\.elf$")


def boot_name(tag, attempt):
    """A boot attempt's files without their extension (BOOT_FILE)."""
    return f"{tag}-boot{attempt}"


def qmp_socket(name):
    """The QMP socket of a boot attempt: /tmp/gen_emu_<pid>_<hash of its name>.sock. The name holds the capture's
    whole Recipe, too long for a UNIX socket's path (104 bytes on macOS)."""
    return f"/tmp/gen_emu_{os.getpid()}_{hashlib.sha1(name.encode()).hexdigest()[:12]}.sock"


def remove_boot_files(out, tag, ext):
    """Removes the capture `tag`'s boot files with extension `ext` from `out`."""
    for name in os.listdir(out):
        m = BOOT_FILE.match(name)
        if m and m["tag"] == tag and m["ext"] == ext:
            with contextlib.suppress(FileNotFoundError):
                os.remove(os.path.join(out, name))


def pid_running(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def clear_dead_captures(out):
    """Removes what a killed capture left in `out`: its raw dumps (up to 100 MB each), its copy of the firmware, and
    the WAV it claimed if that is still empty. Only for captures whose process is gone; their run logs stay."""
    for name in os.listdir(out):
        m = BOOT_FILE.match(name)
        if not m:
            m = CAPTURE_ELF.match(name)
            if m and not pid_running(int(m["pid"])):
                log(f"removing {name}: its capture (pid {m['pid']}) was killed")
                with contextlib.suppress(FileNotFoundError):
                    os.remove(os.path.join(out, name))
            continue
        if m["ext"] != "s32" or pid_running(int(m["pid"])):
            continue
        log(f"removing {name}: its capture (pid {m['pid']}) was killed")
        with contextlib.suppress(FileNotFoundError):
            os.remove(os.path.join(out, name))
            wav = os.path.join(out, m["stem"] + ".wav")
            if os.path.getsize(wav) == 0:
                os.remove(wav)


NO_RENDER = "build generator_render with ./dbt test --generator -n, or pass --render"
HARNESS_BUILD = os.path.join(REPO, "build", "tests")  # ./dbt test --generator's build
HARNESS_RENDER = os.path.join(
    HARNESS_BUILD, "spec", "generator", "Debug", "generator_render"
)
# What generator_render builds from (tests/spec/generator/CMakeLists.txt): these folders, and the harness's headers
# and CMakeLists.txt beside its specs (GENERATOR_HARNESS; the *_spec.cpp files aren't in it).
GENERATOR_SOURCES = tuple(
    os.path.join(REPO, d)
    for d in (
        "src/deluge/generative/core",
        "src/deluge/generative/perform",
        "src/deluge/generative/runner",
        "tests/spec/generator/render",
    )
)
GENERATOR_HARNESS = os.path.join(REPO, "tests/spec/generator")


def find_render(args):
    """--render, or the newest generator_render the harness build made (./dbt test --generator -n)."""
    return args.render or theory_review.find_render()


def require_render(render):
    """Exits unless `render` is a program."""
    if not (os.path.isfile(render) and os.access(render, os.X_OK)):
        sys.exit(f"--render {render}: no such program")


def generator_source_files():
    """Every file generator_render builds from: GENERATOR_SOURCES' trees, and GENERATOR_HARNESS's headers and
    CMakeLists.txt."""
    files = [
        os.path.join(root, f)
        for d in GENERATOR_SOURCES
        for root, _, names in os.walk(d)
        for f in names
    ]
    files += glob.glob(os.path.join(GENERATOR_HARNESS, "*.h"))
    files.append(os.path.join(GENERATOR_HARNESS, "CMakeLists.txt"))
    return files


def newest_generator_source():
    """(mtime, path) of the newest file generator_render builds from."""
    return max((os.path.getmtime(f), f) for f in generator_source_files())


def log_render(render):
    """Logs which generator_render the check reads, and warns if the Generator's source changed since it was built:
    it may not play what a capture built now plays."""
    built = os.path.getmtime(render)
    log(
        "harness render: {} (built {})".format(
            render, time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(built))
        )
    )
    changed, path = newest_generator_source()
    if changed > built:
        log(
            f"WARNING {os.path.relpath(path, REPO)} changed after it was built: rebuild it (./dbt test --generator -n)"
        )


def bar_seconds(tempo):
    """A 4/4 bar at `tempo` BPM, in seconds."""
    return 240.0 / tempo


def render_command(render, args, out, bars):
    """generator_render with the capture's Recipe (the same flags), --midi-only, for `bars` bars."""
    cmd = [render, "--genre", args.genre, "--seed", str(args.seed)]
    if args.entropy:
        cmd += ["--entropy", str(args.entropy)]
    for m in MACROS:
        if getattr(args, m) is not None:
            cmd += ["--" + m, str(getattr(args, m))]
    for flag in ("key", "scale"):
        if getattr(args, flag) is not None:
            cmd += ["--" + flag, getattr(args, flag)]
    if args.tempo is not None:
        cmd += ["--tempo", f"{args.tempo:g}"]
    if args.swing is not None:
        cmd += ["--swing", str(args.swing)]
    return cmd + end_flags(args) + ["--bars", str(bars), "--midi-only", "--out", out]


def end_flags(args):
    """--end-after-drop N or --end-at-bar BAR as given, for generator_render (which takes the same)."""
    if getattr(args, "end_after_drop", None):
        return ["--end-after-drop", str(args.end_after_drop)]
    if getattr(args, "end_at_bar", None) is not None:
        return ["--end-at-bar", str(args.end_at_bar)]
    return []


def drop_bars(sections):
    """The bars where a Drop starts (a Groove a Build lands on), from a Piece's (bar, label) Section markers."""
    return [
        bar
        for (_, before), (bar, label) in itertools.pairwise(sections)
        if label.startswith("GROOVE") and before.startswith("BUILD")
    ]


EndPress = collections.namedtuple("EndPress", "bar seconds ends_bar ends_seconds")


def end_press(render, args):
    """Where a capture with --end-after-drop or --end-at-bar holds the End pad, as an EndPress: the bar (from PLAY,
    bar 0) and its seconds, and the bar and seconds where the piece ends (None if it doesn't end in --seconds). From
    the harness render with the same End, which arms it on that bar's first step. None without End. Raises
    ValueError when the Drop doesn't come, or the bar is past --seconds."""
    if not end_flags(args):
        return None
    tempo = args.tempo
    if tempo is None:
        tempo = float(render_piece(render, args, 1).recipe["tempo"])
    bars = bars_for(args.seconds, tempo)
    piece = render_piece(render, args, bars)
    bar_s = bar_seconds(float(piece.recipe["tempo"]))
    if args.end_at_bar is not None:
        bar = args.end_at_bar
    else:
        drops = drop_bars(piece.sections)
        if len(drops) < args.end_after_drop:
            raise ValueError(
                f"--end-after-drop {args.end_after_drop}: the piece has {len(drops)} Drop(s) in --seconds "
                f"{args.seconds:g} ({bars} bars); raise --seconds"
            )
        bar = drops[args.end_after_drop - 1]
    if bar * bar_s + END_HOLD_MS / 1000 >= args.seconds:
        raise ValueError(
            f"End at bar {bar} ({bar * bar_s:g} s) is past --seconds {args.seconds:g}"
        )
    ended = (
        piece.sections
        and piece.sections[-1][1].startswith("OUTRO")
        and piece.bars < bars
    )
    ends = piece.bars if ended else None
    return EndPress(bar, bar * bar_s, ends, None if ends is None else ends * bar_s)


def bars_for(seconds, tempo):
    """Bars to render for `seconds` of Perform at `tempo` BPM: one more than they cover."""
    bars = math.ceil(seconds / bar_seconds(tempo)) + 1
    if bars > MAX_BARS:
        raise ValueError(
            f"{seconds:g} s at {tempo:g} BPM is {bars} bars; generator_render renders at most {MAX_BARS}"
        )
    return bars


def render_piece(render, args, bars):
    """The harness render of this capture's Recipe for `bars` bars, as a theory_review Piece."""
    with tempfile.TemporaryDirectory() as tmp:
        prefix = os.path.join(tmp, "check")
        done = subprocess.run(
            render_command(render, args, prefix, bars),
            capture_output=True,
            text=True,
            check=False,
        )
        if done.returncode != 0:
            raise RuntimeError(
                f"generator_render failed: {(done.stderr or done.stdout).strip()}"
            )
        with open(prefix + ".mid", "rb") as fh:
            return theory_review.piece_from_midi(fh.read())


def harness_piece(render, args):
    """(parts, seconds a step) of the harness render of this capture's Recipe, for --seconds at its tempo (--tempo,
    or the tempo it rolls, from a one-bar render): theory_review Parts in the genre's order."""
    tempo = args.tempo
    if tempo is None:
        tempo = float(render_piece(render, args, 1).recipe["tempo"])
    piece = render_piece(render, args, bars_for(args.seconds, tempo))
    return piece.parts, bar_seconds(float(piece.recipe["tempo"])) / STEPS_PER_BAR


def fresh_harness_piece(args):
    """harness_piece with a generator_render built from the source now: its target alone in the harness build,
    holding a lock there so captures beside this one take turns. Run right after build_capture, so the check reads
    the notes the firmware plays even after a table edit. ./dbt test and check_all.py build the same tree without the
    lock: don't run them beside a checked capture."""
    os.makedirs(HARNESS_BUILD, exist_ok=True)
    with open(os.path.join(HARNESS_BUILD, "gen_emu.lock"), "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if not os.path.exists(os.path.join(HARNESS_BUILD, "CMakeCache.txt")):
            dbt(
                f"cmake -S tests -B {shlex.quote(HARNESS_BUILD)} -G 'Ninja Multi-Config'"
            )
        dbt(
            f"cmake --build {shlex.quote(HARNESS_BUILD)} --config Debug --target generator_render_cli"
        )
        log_render(HARNESS_RENDER)
        return harness_piece(HARNESS_RENDER, args)


def read_wav(path):
    """The S32LE stereo frames of a capture's WAV."""
    with wave.open(path, "rb") as w:
        shape = (w.getnchannels(), w.getsampwidth(), w.getframerate())
        if shape != (2, 4, RATE):
            raise ValueError(
                "{} channels, {}-byte samples at {} Hz: not a gen_emu capture".format(
                    *shape
                )
            )
        return w.readframes(w.getnframes())


def dbt(command):
    log("./dbt exec " + command)
    subprocess.run([os.path.join(REPO, "dbt"), "exec", command], cwd=REPO, check=True)


def build_capture(args):
    """Builds the capture firmware in --build-dir and returns its ELF. The Recipe isn't in the build (capture_one
    patches it into a copy), so a new Recipe builds nothing: only a source change does."""
    build = os.path.abspath(args.build_dir)
    cache = os.path.join(build, "CMakeCache.txt")
    configured = False
    if os.path.exists(cache):
        with open(cache) as fh:
            configured = "GENERATIVE_CAPTURE:BOOL=ON" in fh.read().splitlines()
    if (
        not configured
    ):  # a new build dir, or one from before #401 (GENERATIVE_CAPTURE_DIR)
        dbt(
            f"cmake -B {shlex.quote(build)} -S {shlex.quote(REPO)} -G 'Ninja Multi-Config' -DCMAKE_CROSS_CONFIGS:STRING=all "
            "'-DCMAKE_DEFAULT_CONFIGS=Debug;Release' -DGENERATIVE_CAPTURE:BOOL=ON -UGENERATIVE_CAPTURE_DIR"
        )
    dbt(f"cmake --build {shlex.quote(build)} --config Release --target deluge")
    return os.path.join(build, "Release", "deluge.elf")


# -- driving the emulator ----------------------------------------------------


class Emu:
    """One delugemu run under icount. The VM moves only in run_to(); every input lands at a fixed virtual time."""

    def __init__(self, args, elf, out, name, *, card=None, snapshot=True):
        if not snapshot and card is None:
            raise ValueError(
                "snapshot=False needs a card clone: it would write to the master card image"
            )
        sys.path.insert(0, os.path.join(args.delugemu, "pylib"))
        from delugemu import DelugeSession
        from delugemu._controls import CONTROLS

        self.controls = CONTROLS
        self.dump = os.path.join(out, name + ".s32")
        if os.path.exists(self.dump):
            os.unlink(self.dump)
        self.t = 0
        extra = ("-icount", "shift=2,sleep=off", "-S") + (
            ("-snapshot",) if snapshot else ()
        )
        self.s = DelugeSession.launch(
            firmware=os.path.abspath(elf),
            sd=os.path.abspath(card or args.card),
            repo_root=args.delugemu,
            env={"DELUGEMU_QEMU_BIN": args.qemu, "DELUGEMU_SSIF_DUMP": self.dump},
            sock=qmp_socket(name),
            run_log=os.path.join(out, name + ".log"),
            extra_qemu=extra,
        )

    def close(self):
        self.s.close()

    def run_to(self, ns, each_step=None):
        while self.t < ns:
            self.t = min(ns, self.t + STEP)
            self.s.run_until(self.t, timeout=600)
            if each_step:
                each_step(self.t)

    def wait(self, ms):
        self.run_to(self.t + ms * MS)

    def button(self, name, down):
        c = self.controls[name]
        self.s.button(c["col"], c["row"], down)

    def tap(self, name, hold_ms=150, settle_ms=300):
        self.button(name, True)
        self.wait(hold_ms)
        self.button(name, False)
        self.wait(settle_ms)

    def shift_tap(self, name):
        self.button("SHIFT", True)
        self.wait(100)
        self.tap(name, settle_ms=100)
        self.button("SHIFT", False)
        self.wait(300)

    def led(self, name):
        return bool(self.s.read_leds().get(name))

    def oled(self):
        return hashlib.sha1(repr(self.s.read_oled()).encode()).hexdigest()

    def booted(self, limit_s=60):
        """Runs until the OLED is on and shows something, then 5 s more for the card and the boot song."""
        while not (self.s.oled_on() and any(any(row) for row in self.s.read_oled())):
            if self.t > limit_s * SEC:
                return False
            self.wait(100)
        self.wait(5000)
        return True

    def card_quiet(self, quiet_ms=1000, limit_s=60):
        """Runs until the firmware has issued no SD command for `quiet_ms` of virtual time. False if the card is
        still busy `limit_s` of virtual time after the call."""
        begin = self.t
        last, since = self.s.sd_command_count(), self.t
        while self.t - since < quiet_ms * MS:
            if self.t - begin >= limit_s * SEC:
                return False
            self.wait(100)
            count = self.s.sd_command_count()
            if count != last:
                last, since = count, self.t
        return True

    def dump_size(self):
        return os.path.getsize(self.dump) if os.path.exists(self.dump) else 0

    def audio(self, start, seconds):
        with open(self.dump, "rb") as fh:
            fh.seek(start)
            return fh.read(int(seconds * RATE) * FRAME)


def clone_card(master, dst):
    """Clones `master` to `dst` (APFS copy-on-write on macOS). Refuses to write over `master`."""
    if os.path.realpath(master) == os.path.realpath(dst):
        raise ValueError("refusing to run on the master card image")
    if os.path.exists(dst):
        os.remove(dst)
    subprocess.run(["cp", "-c", master, dst], check=True)


def boot(args, elf, out, tag, *, card=None, snapshot=True):
    """Launches and boots, relaunching a blank boot up to test_port_emu.BOOT_TRIES times."""
    for attempt in range(1, test_port_emu.BOOT_TRIES + 1):
        if card is not None and not snapshot:
            clone_card(args.card, card)
        if card is not None or not snapshot:
            emu = Emu(
                args, elf, out, boot_name(tag, attempt), card=card, snapshot=snapshot
            )
        else:
            emu = Emu(args, elf, out, boot_name(tag, attempt))
        try:
            booted = emu.booted()
        except BaseException:
            emu.close()  # or qemu outlives the run
            raise
        if booted:
            log(f"booted (attempt {attempt}) at {emu.t / SEC:.1f} s virtual")
            return emu
        log(f"boot {attempt} came up blank, retrying")
        emu.close()
    raise RuntimeError(f"no boot in {test_port_emu.BOOT_TRIES} tries")


def enter_perform(emu):
    """SHIFT+SONG to Song view, SHIFT+SONG again for GENERATIVE, then PERFORM FROM -> GENRE -> TECHNO -> the default
    genre -> DISCARD SONG? -> OK (spec §14.1, §14.3): GENRE opens on the default genre's row and submenu. A capture
    build plays its own genre whatever row is picked."""
    emu.shift_tap("SESSION_VIEW")
    emu.wait(1500)  # the zoom out to Song view; SHIFT+SONG needs no UI mode active
    emu.shift_tap("SESSION_VIEW")
    for _ in range(5):  # PERFORM, GENRE, TECHNO, the genre, OK
        emu.tap("SELECT_ENC")
    return emu.card_quiet(limit_s=30)


def check_part_focus(emu, check):
    """Part focus on the SONG page (spec §13.5, §19): AFFECT ENTIRE is lit at the Recipe level, dark while a part's row
    is held, and SONG stays lit and CLIP dark over the stock LED updates that binding the knobs makes."""
    check("AFFECT ENTIRE lit at the Recipe level", emu.led("AFFECT_ENTIRE"))
    emu.s.pad_event(0, 7, True)  # row 1 (firmware y 7): the first part
    emu.wait(300)
    check(
        "holding a part row focuses it (AFFECT ENTIRE dark, SONG lit, CLIP dark)",
        not emu.led("AFFECT_ENTIRE")
        and emu.led("SESSION_VIEW")
        and not emu.led("CLIP_VIEW"),
    )
    emu.s.pad_event(0, 7, False)
    emu.wait(300)
    check("releasing it goes back to the Recipe level", emu.led("AFFECT_ENTIRE"))


SIDEBAR_MUTE_X = 16  # MUTE/LAUNCH; part 1 is the top row, firmware y 7 (spec §13.5)
MUTE_ROWS = 6


def mute_parts(emu, parts):
    """Mutes each sidebar part (1-6) with SHIFT held, so each lands on the next undecided step (spec §13.4)."""
    if not parts:
        return
    emu.button("SHIFT", True)
    emu.wait(50)
    for part in parts:
        emu.s.pad_event(SIDEBAR_MUTE_X, 8 - part, True)
        emu.wait(50)
        emu.s.pad_event(SIDEBAR_MUTE_X, 8 - part, False)
        emu.wait(50)
    emu.button("SHIFT", False)


def press_end(emu):
    """Holds the End pad END_HOLD_MS, past the 600 ms that arms End, and lets go."""
    emu.s.pad_event(*END_PAD, True)
    emu.wait(END_HOLD_MS)
    emu.s.pad_event(*END_PAD, False)


def run_for_audio(emu, start, seconds, each_step=None):
    """Runs on until the dump holds `seconds` of audio from `start` (bytes), at most TAIL_LIMIT_S of virtual time:
    the emulator's audio lags its virtual clock by a few blocks. Returns the milliseconds it ran on."""
    need = int(seconds * RATE) * FRAME
    began = emu.t
    while emu.dump_size() - start < need and emu.t - began < TAIL_LIMIT_S * SEC:
        emu.run_to(emu.t + 10 * MS, each_step)
    return (emu.t - began) // MS


def check_perform_plays(
    emu, seconds, check, mutes=(), check_silence=True, end_at=None, ends_at=None
):
    """PLAY, then `seconds` of Perform: sound all the way (unless not `check_silence`: a capture checks its audio by
    the harness's notes instead) and an OLED that keeps moving. Returns the rendered PCM. Each part in `mutes` is muted
    right after PLAY. `end_at`: seconds after PLAY to hold the End pad from; `ends_at`: seconds after PLAY where the
    piece then ends, after which the OLED shows STOPPED and stays still."""
    check(
        "entered Perform on the SONG page (BACK and SONG lit, CLIP dark)",
        emu.led("BACK") and emu.led("SESSION_VIEW") and not emu.led("CLIP_VIEW"),
    )
    check("stopped on entry", not emu.led("PLAY"))
    check_part_focus(emu, check)
    # delugemu writes the dump unbuffered (rza1l_ssif.c fflushes each write), and the VM is stopped between
    # run_until steps, so the dump's size now is where the audio after PLAY starts.
    start = emu.dump_size()
    played = emu.t
    emu.button("PLAY", True)
    emu.wait(150)
    emu.button("PLAY", False)
    check("PLAY starts Perform", emu.led("PLAY"))
    mute_parts(emu, list(mutes))
    frames = []
    step = lambda t: frames.append((t, emu.oled()))
    stop = emu.t + int(seconds * SEC)
    if end_at is not None:
        emu.run_to(played + int(end_at * SEC), each_step=step)
        press_end(emu)
    emu.run_to(stop, each_step=step)
    tail = run_for_audio(emu, start, seconds, step)
    if tail:
        log(f"ran {tail} ms past --seconds for the audio the dump lagged behind")
    pcm = emu.audio(start, seconds)
    if check_silence:
        silences = audio_failures(pcm, seconds)
        for failure in silences:
            check(failure, False)
        check("Perform sounds all the way through", not silences)
    if ends_at is not None:
        frames = [f for f in frames if f[0] <= played + int(ends_at * SEC)]
    freezes = freeze_failures(frames)
    for failure in freezes:
        check(failure, False)
    check("the OLED keeps moving", not freezes)
    return pcm


def write_wav(path, pcm):
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(4)
        w.setframerate(RATE)
        w.writeframes(pcm)


# -- commands ----------------------------------------------------------------


def warn_pin(args):
    installed = installed_versions(args.qemu, args.delugemu)
    for warning in pin_warnings(read_pin(), installed):
        log("WARNING " + warning)
    return installed


def leak(args):
    """The leak check. Returns 0 if it passes."""
    elf = args.testport_elf or test_port_emu.build_testport()
    out = os.path.join(args.out, "leak")
    script = deluge_probe.read_script(args.cycle_script)
    failed_expects = 0
    log(
        f"leak check: {args.cycles} Perform enter/leave cycles of "
        f"{os.path.basename(args.cycle_script)} on {elf}"
    )
    try:
        with test_port_emu.EmuRun(
            elf, out, args.delugemu, args.qemu, args.card, tag="leak"
        ) as emu:
            for n in range(1, args.cycles + 1):
                failed, total = deluge_probe.run_script(
                    emu.port,
                    script,
                    out=lambda line: None if line.startswith("PASS") else log(line),
                )
                log(f"leak cycle {n}: {total} expects, {failed} failed")
                failed_expects += failed
            events = list(emu.port.events)
    except (RuntimeError, TimeoutError) as exc:
        log(f"FAIL leak check: {exc}")
        return 1
    lines, failures = leak_report(mem_cycles(events), args.cycles)
    for line in lines:
        log("  " + line)
    if failed_expects:
        failures.append(f"{failed_expects} expects failed in the cycles")
    for failure in failures:
        log("FAIL " + failure)
    log(
        "leak check: {} (logs in {})".format(
            "PASS" if not failures else f"FAIL, {len(failures)} problems", out
        )
    )
    return 1 if failures else 0


SMOKE_RELOAD_SECONDS = 2.0


def smoke(args):
    installed = warn_pin(args)
    os.makedirs(args.out, exist_ok=True)
    seconds = args.bars * SECONDS_PER_BAR
    failed = total = 0

    def check(name, ok):
        nonlocal failed, total
        failed += 0 if ok else 1
        total += 1
        log("{} {}".format("PASS" if ok else "FAIL", name))

    card = os.path.join(args.out, "smoke.img")
    try:
        emu = boot(args, args.elf, args.out, "smoke", card=card, snapshot=False)
        try:
            check("boots", True)
            check("Perform's song builds", enter_perform(emu))
            check_perform_plays(emu, seconds, check)
            emu.tap("PLAY")
            check("PLAY stops", not emu.led("PLAY"))
            emu.tap("BACK", hold_ms=700)  # held: the leave menu (spec §13.8)
            # One unmarked piece played: LEAVE, KEEP: LAST (selected first), NOTHING, CANCEL.
            emu.tap("SELECT_ENC")  # LAST: leave and Capture the played piece
            captured = emu.card_quiet(quiet_ms=2000, limit_s=60)
            check(
                "BACK held, then LAST, leaves Perform and Captures",
                captured and not emu.led("BACK") and not emu.led("CLIP_VIEW"),
            )
            # Reload the saved Capture song through stock LOAD (opens on currentSong->name).
            emu.tap("LOAD")
            emu.wait(1000)
            emu.card_quiet(limit_s=30)
            emu.tap("LOAD")
            reloaded = emu.card_quiet(limit_s=60)
            reload_start = emu.dump_size()
            emu.tap("PLAY", settle_ms=150)
            check("reloaded Capture song plays", reloaded and emu.led("PLAY"))
            emu.wait(int(SMOKE_RELOAD_SECONDS * 1000))
            reload_pcm = emu.audio(reload_start, SMOKE_RELOAD_SECONDS)
            reload_silences = audio_failures(reload_pcm, SMOKE_RELOAD_SECONDS)
            for failure in reload_silences:
                check("reloaded Capture song: " + failure, False)
            check("reloaded Capture song sounds", not reload_silences)
            emu.tap("PLAY")
            emu.s.screenshot(os.path.join(args.out, "smoke-end.png"))
        finally:
            emu.close()
        songs = new_song_files(card, args.card)
        check(
            f"Capture wrote one song file ({', '.join(songs) or 'none'})",
            len(songs) == 1,
        )
        if len(songs) == 1:
            try:
                with fat_image.FatImage(card) as fat:
                    song_xml = fat.read_file(f"{SONG_FOLDER}/{songs[0]}").decode()
                tracks, instances, notes = golden_song.capture_song_summary(song_xml)
                check(
                    f"Capture song {songs[0]} holds notes ({tracks} tracks, {instances} instances, {notes} notes)",
                    True,
                )
            except (
                OSError,
                ValueError,
                golden_song.GoldenError,
                ET.ParseError,
            ) as exc:
                check(f"Capture song {songs[0]} holds notes ({exc})", False)
    finally:
        if (
            not getattr(args, "keep_card", False)
            and os.path.exists(card)
            and os.path.realpath(card) != os.path.realpath(args.card)
        ):
            os.remove(card)
    log(f"{total} checks, {failed} failed; logs in {args.out}")
    leak_failed = 0 if args.no_leak else leak(args)
    log(
        "smoke: functional {}/{} passed; leak check {}".format(
            total - failed,
            total,
            "skipped" if args.no_leak else ("FAIL" if leak_failed else "PASS"),
        )
    )
    if failed == 0 and args.update_pin:
        write_pin(installed)
        log(
            "delugemu.pin now {} / {}: commit it".format(
                installed["bundle"], installed["checkout"][:7]
            )
        )
    return 1 if failed or leak_failed else 0


GOLDEN_SCRIPT = os.path.join(HERE, "golden_compose.txt")
GOLDEN_SONG = "SONGS/GOLDEN.XML"  # golden_compose.txt's "save song GOLDEN"
SONG_FOLDER = "SONGS"
RECIPE_FOLDER = "RECIPES"
DIFF_LINES = 40  # of a failing diff, on the console; the whole dumps are in --out


def golden_genre(script_text):
    """The genre golden_compose.txt rolls: its last "pick genre" line's."""
    picks = re.findall(r"^pick genre (\S+)$", script_text, re.MULTILINE)
    if not picks:
        raise ValueError("golden_compose.txt has no pick genre line")
    return picks[-1]


def new_card_files(card, master, folder):
    """Files in `folder` on the card clone `card` that `master` doesn't have: what the run saved."""

    def names(image):
        with fat_image.FatImage(image) as fat:
            return set(fat.listdir(folder)) if fat.exists(folder) else set()

    return sorted(names(card) - names(master))


def new_recipe_files(card, master):
    """The Recipe files on the card clone that the master card hasn't: what the script saved."""
    return new_card_files(card, master, RECIPE_FOLDER)


def new_song_files(card, master):
    """The song files on the card clone that the master card hasn't: what Capture saved."""
    return new_card_files(card, master, SONG_FOLDER)


def golden(args):
    """The firmware check and the Compose and Recipe-file smoke checks (spec §21.3). Returns 0 if they pass."""
    warn_pin(args)
    elf = args.testport_elf or test_port_emu.build_testport()
    with open(GOLDEN_SCRIPT) as fh:
        genre = golden_genre(fh.read())
    with open(golden_song.golden_path(genre)) as fh:
        golden_text = fh.read()
    failed = total = 0

    def check(name, ok):
        nonlocal failed, total
        failed += 0 if ok else 1
        total += 1
        log("{} {}".format("PASS" if ok else "FAIL", name))

    log(f"firmware check: {genre}'s golden case through Compose on {elf}")
    os.makedirs(args.out, exist_ok=True)
    run = test_port_emu.EmuRun(
        elf, args.out, args.delugemu, args.qemu, args.card, keep_card=True, tag="golden"
    )
    try:
        with run as emu:
            results = test_port_emu.run_scripts(
                emu,
                [GOLDEN_SCRIPT],
                out=lambda line: None if ": PASS " in line else log(line),
            )
        _, script_failed, script_total, error, _ = results[0]
        check(
            f"golden_compose.txt: {script_total} expects, {script_failed} failed"
            + (f" ({error})" if error else ""),
            not script_failed and not error,
        )
        if script_failed or error:
            return 1
        with fat_image.FatImage(run.card) as fat:
            song = fat.read_file(GOLDEN_SONG).decode()
            recipes = new_recipe_files(run.card, args.card)
            check(
                f"one Recipe file saved ({', '.join(recipes) or 'none'})",
                len(recipes) == 1,
            )
            if len(recipes) != 1:
                return 1
            recipe = fat.read_file(f"{RECIPE_FOLDER}/{recipes[0]}").decode()
    except (RuntimeError, ValueError, OSError, golden_song.GoldenError) as exc:
        check(f"firmware check ran ({exc})", False)
        return 1
    finally:
        if not args.keep_card and os.path.exists(run.card):
            os.unlink(run.card)
    try:
        problems, song_lines, golden_lines = golden_song.compare(
            golden_text, song, recipe
        )
    except (golden_song.GoldenError, ET.ParseError) as exc:
        check(f"the saved files read ({exc})", False)
        return 1
    for problem in problems:
        log("  " + problem)
    check(f"the Recipe file {recipes[0]} holds {genre}'s golden case", not problems)
    for name, lines in (("song", song_lines), ("golden", golden_lines)):
        with open(os.path.join(args.out, f"{name}-notes.txt"), "w") as fh:
            fh.write("\n".join(lines) + "\n")
    diff = list(
        difflib.unified_diff(
            golden_lines, song_lines, f"{genre} golden", GOLDEN_SONG, lineterm="", n=1
        )
    )
    for line in diff[:DIFF_LINES]:
        log("  " + line)
    if len(diff) > DIFF_LINES:
        log(
            f"  ... {len(diff) - DIFF_LINES} more diff lines: golden-notes.txt and song-notes.txt in {args.out}"
        )
    check(
        f"the saved song's notes are the golden's: {len(song_lines) // 2} notes in the song, "
        f"{len(golden_lines) // 2} in the golden",
        not diff,
    )
    log(f"{total} checks, {failed} failed; logs in {args.out}")
    return 1 if failed else 0


def check_capture_args(args):
    """Exits on arguments the capture would refuse only after its minutes-long build."""
    # The check's render covers the capture: at --tempo, or at the fastest a rolled Recipe can be. --no-check
    # renders nothing.
    try:
        if not args.no_check:
            bars_for(args.seconds, args.tempo if args.tempo is not None else MAX_TEMPO)
    except ValueError as error:
        fastest = (
            ""
            if args.tempo is not None
            else f" (no --tempo, so checked at the fastest tempo a Recipe can have, {MAX_TEMPO} BPM)"
        )
        sys.exit(f"capture: {error}{fastest}; --no-check skips the check's render")
    if getattr(args, "elf", None) and not os.path.isfile(args.elf):
        sys.exit(f"capture: --elf {args.elf}: no such file")
    if args.ab and not args.mute:
        sys.exit("capture --ab needs a part to leave out: --mute PART")
    if args.ab and is_wav_path(args.out):
        sys.exit("capture --ab writes two captures: give --out a folder")


def capture_one(args, elf, mutes, harness=None, end=None):
    """One capture with `mutes` muted. Returns (the WAV's path, why its audio isn't whole). `harness` is
    harness_piece's (parts, seconds a step): the audio is checked as `gen_emu.py check` does. Without it, only its
    length and full-scale runs fail it, and silence is a WARNING. `end`: end_press()'s EndPress, or None."""
    out = (os.path.dirname(args.out) or ".") if is_wav_path(args.out) else args.out
    os.makedirs(out, exist_ok=True)
    clear_dead_captures(out)
    wav = capture_path(args, args.out, mutes)
    tag = capture_tag(wav)
    # Its own copy (the tag holds the pid), so captures from one build dir at once never patch the same file.
    patched = os.path.join(out, tag + ".elf")
    warnings = []
    writing = False
    try:
        write_capture_elf(elf, patched, capture_recipe(args))
        emu = boot(args, patched, out, tag)
        try:
            if not enter_perform(emu):
                raise RuntimeError("Perform's song never finished loading")
            pcm = check_perform_plays(
                emu,
                args.seconds,
                lambda what, ok: ok or warnings.append(what),
                mutes,
                check_silence=False,
                end_at=None if end is None else end.seconds,
                ends_at=None if end is None else end.ends_seconds,
            )
        finally:
            emu.close()
        writing = True
        write_wav(wav, pcm)
    except BaseException:
        if os.path.exists(wav) and (writing or os.path.getsize(wav) == 0):
            os.remove(wav)  # half written, or the name capture_path claimed
        raise
    finally:
        # every boot attempt's raw dump: up to 100 MB each, and nothing reads it again
        remove_boot_files(out, tag, "s32")
        with contextlib.suppress(FileNotFoundError):
            os.remove(patched)
    if harness:
        parts, step_seconds = harness
        verdict = wav_failures(
            pcm,
            args.seconds,
            active_steps(parts, set(mutes)),
            step_seconds,
            quiet_bars(args),
        )
    else:
        verdict = wav_failures(pcm, args.seconds, None, 1.0)
        warnings += audio_failures(pcm, args.seconds)
    for warning in warnings + verdict.warnings:
        log("WARNING " + warning)
    for failure in verdict.failures:
        log("FAIL " + failure)
    kept = warnings + verdict.warnings + verdict.failures
    if not kept:
        remove_boot_files(out, tag, "log")  # a warning or a failure keeps them to read
    log(
        f"{wav} (sha256 {hashlib.sha256(pcm).hexdigest()[:16]}): "
        + (
            "BAD (logs: {})".format(os.path.join(out, boot_name(tag, "*") + ".log"))
            if verdict.failures
            else (
                f"checked {verdict.checked} sounding steps, whole"
                if harness
                else "unchecked"
            )
            + (
                f" (logs: {os.path.join(out, boot_name(tag, '*') + '.log')})"
                if kept
                else ""
            )
        )
    )
    return wav, verdict.failures


def capture(args):
    check_capture_args(args)
    capture_recipe(args)  # an unknown genre, key or scale stops before the build
    warn_pin(args)
    check = not args.no_check
    if check and args.render:
        require_render(args.render)  # before the minutes-long build
    if not check and end_flags(args):
        render = find_render(args)
        if not render:
            sys.exit(
                "capture: End needs generator_render even with --no-check: " + NO_RENDER
            )
        require_render(render)
    elf = args.elf or build_capture(args)
    harness = None
    if check and args.render:
        log_render(args.render)
        harness = harness_piece(args.render, args)
    elif check:
        harness = fresh_harness_piece(args)
    end = None
    if end_flags(args):
        try:
            end = end_press(
                args.render or (HARNESS_RENDER if check else find_render(args)), args
            )
        except ValueError as error:
            sys.exit(f"capture: {error}")
        log(
            f"End: held from bar {end.bar} ({end.seconds:g} s); "
            + (
                f"the piece ends at bar {end.ends_bar} ({end.ends_seconds:g} s)"
                if end.ends_bar is not None
                else "the piece doesn't end in --seconds: raise it to hear the Outro"
            )
        )
    if not args.ab:
        wav, failures = capture_one(args, elf, args.mute or [], harness, end)
        if not args.no_play:
            subprocess.run(["afplay", wav], check=False)
        return 1 if failures else 0
    full, full_failures = capture_one(args, elf, [], harness, end)
    without, failures = capture_one(args, elf, args.mute, harness, end)
    if full_failures or failures:
        return 1
    import wav_levels

    return wav_levels.main(["--skip", "1", "--full", full, without], out=log)


def check_wavs(args):
    """gen_emu.py check: whether each WAV is a whole capture of these arguments. Returns 0 if all are."""
    render = find_render(args)
    if not render:
        sys.exit("check needs generator_render: " + NO_RENDER)
    require_render(render)
    log_render(render)
    parts, step_seconds = harness_piece(render, args)
    steps = active_steps(parts, set(args.mute or []))
    bad = 0
    for path in args.wavs:
        try:
            verdict = wav_failures(
                read_wav(path), args.seconds, steps, step_seconds, quiet_bars(args)
            )
        except (OSError, EOFError, ValueError, wave.Error) as exc:
            verdict = WavVerdict([str(exc)], [], 0)
        for warning in verdict.warnings:
            log(f"WARNING {path}: {warning}")
        if verdict.failures:
            bad += 1
            log(f"BAD {path}: " + "; ".join(verdict.failures))
        else:
            log(
                f"ok {path} ({args.seconds:g} s, {verdict.checked} sounding steps checked)"
            )
    return 1 if bad else 0


def add_leak_args(p):
    p.add_argument(
        "--cycles",
        type=int,
        default=LEAK_CYCLES,
        help=f"leak check: Perform enter/leave cycles (at least {WARMUP_CYCLES + 1})",
    )
    p.add_argument(
        "--cycle-script",
        default=os.path.join(HERE, "perform_mem_cycle.txt"),
        help="leak check: the probe script one cycle runs (default perform_mem_cycle.txt). It must enter and leave "
        "Perform once, so it gets one gen_mem before_entry and one after_leave event",
    )
    p.add_argument(
        "--testport-elf",
        help="leak check: this ENABLE_SYSEX_TEST firmware instead of building build-testport/",
    )


def add_recipe_args(p):
    """The capture's Recipe, length and mutes: capture and check take the same."""
    p.add_argument("--genre", default="techno-acid")
    p.add_argument("--seed", type=uint32, default=1)
    p.add_argument(
        "--entropy",
        type=uint32,
        default=0,
        help="what the genre's facts are rolled from",
    )
    for m in MACROS:
        p.add_argument(
            "--" + m, type=int, choices=range(51), metavar="0-50", help="OLED points"
        )
    p.add_argument("--key", help="C, C#, Db ... B")
    p.add_argument("--scale", help="minor, phrygian, harmonic-minor ...")
    p.add_argument("--tempo", type=bpm, help="BPM, 20 to 999")
    p.add_argument("--swing", type=int, choices=range(-49, 50), metavar="-49..49")
    p.add_argument(
        "--seconds", type=float, default=60.0, help="Perform time to capture"
    )
    p.add_argument(
        "--mute",
        type=int,
        action="append",
        choices=range(1, MUTE_ROWS + 1),
        metavar="PART",
        help="mute this part (sidebar row, 1-6) from the first step; repeatable",
    )
    p.add_argument(
        "--end-after-drop",
        type=drop_count,
        nargs="?",
        const=1,
        metavar="N",
        help="press End from the Nth Drop's first bar (default the first), so the Outro follows that Drop's phrase "
        "and the piece ends",
    )
    p.add_argument(
        "--end-at-bar",
        type=bar_number,
        metavar="BAR",
        help="press End from this bar (bar 0 at PLAY): the Outro follows from the next phrase line",
    )
    p.add_argument(
        "--quiet-bars",
        type=bar_set,
        action="append",
        metavar="BARS",
        help="bars from PLAY (bar 0) where the check expects silence, as 47 or 3,40-42: an FX part alone, whose "
        "riser starts from silence; repeatable",
    )
    p.add_argument(
        "--render",
        help="the generator_render the check renders with (default: capture builds it from the source now; check "
        "takes the newest under build/tests/)",
    )


def parser():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    sub = p.add_subparsers(dest="command", required=True)
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument(
        "--card", default=os.path.join(HOME, "source", "delugemu-sd", "deluge_sd.img")
    )
    common.add_argument("--delugemu", default=os.path.join(HOME, "source", "delugemu"))
    common.add_argument("--qemu", default=QEMU)

    s = sub.add_parser(
        "smoke", parents=[common], help="boot, enter Perform, play N bars"
    )
    s.add_argument(
        "--elf", default=os.path.join(REPO, "build", "Release", "deluge.elf")
    )
    s.add_argument(
        "--bars",
        type=int,
        default=8,
        help="at least this many bars of Perform (2 s each)",
    )
    s.add_argument("--out", default=os.path.join(REPO, "build", "delugemu-smoke"))
    s.add_argument(
        "--update-pin",
        action="store_true",
        help="record the installed delugemu once the checks pass",
    )
    s.add_argument("--no-leak", action="store_true", help="skip the leak check")
    s.add_argument(
        "--keep-card",
        action="store_true",
        help="leave the card clone in --out after the run",
    )
    add_leak_args(s)
    lk = sub.add_parser(
        "leak", parents=[common], help="the memory check over enter/leave cycles"
    )
    add_leak_args(lk)
    lk.add_argument("--out", default=os.path.join(REPO, "build", "delugemu-smoke"))

    g = sub.add_parser(
        "golden",
        parents=[common],
        help="the firmware check: a golden's case through Compose, its saved song's notes against the golden",
    )
    g.add_argument(
        "--testport-elf",
        help="this ENABLE_SYSEX_TEST firmware instead of building build-testport/",
    )
    g.add_argument("--out", default=os.path.join(REPO, "build", "delugemu-golden"))
    g.add_argument(
        "--keep-card",
        action="store_true",
        help="keep the card clone (golden.img in --out)",
    )

    recipe = argparse.ArgumentParser(add_help=False)
    add_recipe_args(recipe)
    c = sub.add_parser(
        "capture",
        parents=[common, recipe],
        help="build, capture, check and play a WAV of Perform",
    )
    c.add_argument(
        "--ab",
        action="store_true",
        help="capture without and then with --mute, and print both levels and the muted parts' estimated level",
    )
    c.add_argument("--no-play", action="store_true")
    c.add_argument(
        "--no-check",
        action="store_true",
        help="skip the check (gen_emu.py check) of the WAV against the harness render; the length and a corrupt "
        "run are still checked, and silence is a WARNING",
    )
    c.add_argument("--build-dir", default=os.path.join(REPO, "build-capture"))
    c.add_argument(
        "--elf",
        help="capture with this capture firmware (a --build-dir's Release/deluge.elf) and build nothing; "
        "--build-dir is then ignored, and the check renders with the current checkout's harness, which may differ "
        "from the one the ELF was built from (--no-check skips it)",
    )
    c.add_argument(
        "--out",
        default=os.path.join(REPO, "build-capture", "captures"),
        help="a folder (the WAV is named after the arguments, never overwriting one) or a .wav file path "
        "(overwritten: don't give two captures that may run at once the same one)",
    )
    k = sub.add_parser(
        "check",
        parents=[recipe],
        help="is a capture whole: its length, sound after each note the harness starts, no corrupt run",
    )
    k.add_argument(
        "wavs", nargs="+", metavar="WAV", help="captures made with these arguments"
    )
    return p


def check_args(args):
    """Exits saying why when the arguments can't work, before anything builds or boots."""
    if getattr(args, "cycles", LEAK_CYCLES) < WARMUP_CYCLES + 1:
        sys.exit(f"--cycles must be at least {WARMUP_CYCLES + 1}")
    script = getattr(args, "cycle_script", None)
    if script is not None and not os.path.isfile(script):
        sys.exit(f"--cycle-script {script}: no such file")
    if (
        getattr(args, "end_after_drop", None)
        and getattr(args, "end_at_bar", None) is not None
    ):
        sys.exit("--end-after-drop and --end-at-bar: give one")


def main():
    args = parser().parse_args()
    if hasattr(args, "out"):
        args.out = os.path.abspath(args.out)  # run.sh runs in the delugemu checkout
    check_args(args)
    command = {
        "smoke": smoke,
        "leak": leak,
        "golden": golden,
        "capture": capture,
        "check": check_wavs,
    }[args.command]
    sys.exit(command(args))


if __name__ == "__main__":
    main()
