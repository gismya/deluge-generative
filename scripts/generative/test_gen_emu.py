"""Tests for gen_emu.py's checks and its capture build, without an emulator.

The emulator runs themselves are checked by running gen_emu.py smoke against a release build (spec §21.3).

    python3 scripts/generative/test_gen_emu.py
"""

import math
import os
import struct
import sys
import tempfile
import unittest
from typing import ClassVar
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gen_emu

RATE = gen_emu.RATE
SEC = gen_emu.SEC


def pcm(seconds_loud, seconds_silent=0.0, then_loud=0.0):
    """S32LE stereo: a 110 Hz tone, then digital silence, then the tone again."""
    out = bytearray()
    for length, loud in (
        (seconds_loud, True),
        (seconds_silent, False),
        (then_loud, True),
    ):
        for i in range(int(length * RATE)):
            v = int(0.3 * 2**31 * math.sin(2 * math.pi * 110 * i / RATE)) if loud else 0
            out += struct.pack("<ii", v, v)
    return bytes(out)


class AudioChecks(unittest.TestCase):
    def test_sound_all_the_way_through_passes(self):
        self.assertEqual(gen_emu.audio_failures(pcm(4.0), seconds=4.0), [])

    def test_a_silent_perform_fails(self):
        failures = gen_emu.audio_failures(pcm(0.0, 4.0), seconds=4.0)
        self.assertEqual(failures, ["silent from 0.0 s to 4.0 s"])

    def test_a_dropout_fails_where_it_happens(self):
        failures = gen_emu.audio_failures(pcm(1.5, 1.0, 1.5), seconds=4.0)
        self.assertEqual(failures, ["silent from 1.5 s to 2.5 s"])

    def test_audio_that_stops_short_fails(self):
        failures = gen_emu.audio_failures(pcm(2.0), seconds=4.0)
        self.assertEqual(failures, ["only 2.0 s of audio rendered, wanted 4.0 s"])


class FreezeChecks(unittest.TestCase):
    def frames(self, changes_until_s, seconds):
        """OLED samples every 100 ms that change each beat (every 0.5 s) until changes_until_s, then stay put."""
        out = []
        for i in range(int(seconds * 10)):
            t = i * SEC // 10
            beat = min(t, int(changes_until_s * SEC)) // (SEC // 2)
            out.append((t, f"frame{beat}"))
        return out

    def test_an_oled_that_moves_each_beat_passes(self):
        self.assertEqual(gen_emu.freeze_failures(self.frames(8.0, 8.0)), [])

    def test_an_oled_that_stops_moving_fails(self):
        self.assertEqual(
            gen_emu.freeze_failures(self.frames(3.0, 8.0)),
            ["the OLED stopped changing at 3.0 s (frozen?)"],
        )


class FakeCard:
    """A paused VM whose card issues `commands_per_step` SD commands each time it runs."""

    def __init__(self, busy_until_ns):
        self.vclock = 0
        self.count = 0
        self.busy_until_ns = busy_until_ns

    def run_until(self, ns, timeout=None):
        if self.vclock < self.busy_until_ns:
            self.count += 1
        self.vclock = ns

    def sd_command_count(self):
        return self.count


def fake_emu(card):
    emu = gen_emu.Emu.__new__(gen_emu.Emu)
    emu.s = card
    emu.t = 0
    return emu


class CardQuiet(unittest.TestCase):
    def test_a_card_that_goes_quiet_passes_after_a_quiet_second(self):
        emu = fake_emu(FakeCard(busy_until_ns=3 * SEC))
        self.assertTrue(emu.card_quiet(limit_s=10))
        self.assertLess(emu.t, 5 * SEC)

    def test_a_card_that_never_goes_quiet_fails_at_the_limit(self):
        emu = fake_emu(FakeCard(busy_until_ns=10**18))
        self.assertFalse(emu.card_quiet(limit_s=10))
        self.assertLessEqual(emu.t, 10 * SEC + gen_emu.STEP)


class Pin(unittest.TestCase):
    PIN: ClassVar[dict] = {
        "bundle": "nightly-20261102-23d1e74",
        "checkout": "23d1e7455028e393f2b1c08b9f303b4481de5d44",
    }

    def test_the_pinned_delugemu_gives_no_warning(self):
        self.assertEqual(gen_emu.pin_warnings(self.PIN, dict(self.PIN)), [])

    def test_a_different_bundle_or_checkout_warns(self):
        installed = {
            "bundle": "nightly-20261201-abcdef0",
            "checkout": "abcdef0" + "0" * 33,
        }
        warnings = gen_emu.pin_warnings(self.PIN, installed)
        self.assertEqual(len(warnings), 2)
        self.assertIn("nightly-20261201-abcdef0", warnings[0])
        self.assertIn("nightly-20261102-23d1e74", warnings[0])
        self.assertIn("checkout", warnings[1])

    def test_nothing_pinned_says_how_to_pin(self):
        self.assertEqual(
            gen_emu.pin_warnings({}, dict(self.PIN)),
            [
                "no delugemu is pinned yet: run gen_emu.py smoke --update-pin once the checks pass"
            ],
        )

    def test_the_repo_records_a_pin(self):
        pin = gen_emu.read_pin()
        self.assertRegex(pin["bundle"], r"^nightly-\d{8}-[0-9a-f]{7}$")
        self.assertRegex(pin["checkout"], r"^[0-9a-f]{40}$")

    def test_update_pin_writes_what_is_installed(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "delugemu.pin")
            gen_emu.write_pin(self.PIN, path)
            self.assertEqual(gen_emu.read_pin(path), self.PIN)


class CaptureBuild(unittest.TestCase):
    def test_the_header_sets_genre_seed_and_each_override_given(self):
        args = gen_emu.parser().parse_args(
            [
                "capture",
                "--genre",
                "techno-acid",
                "--seed",
                "7",
                "--energy",
                "40",
                "--key",
                "C#",
                "--scale",
                "phrygian",
                "--tempo",
                "131.5",
                "--swing",
                "-10",
            ]
        )
        self.assertEqual(
            gen_emu.capture_header(args),
            "// Generated by scripts/generative/gen_emu.py capture. Not committed.\n"
            "#define GENERATIVE_CAPTURE_OVERRIDES "
            '{.genre = "techno-acid", .entropy = 0u, .seed = 7u, .macroPoints = {40, -1, -1, -1}, '
            ".tempo = 13150, .keyRoot = 1, .scale = 3, .swing = -10}\n",
        )

    def test_it_takes_the_ranges_the_render_takes(self):
        for argv in (
            ["--seed", "-1"],
            ["--seed", str(2**32)],
            ["--entropy", str(2**32)],
            ["--tempo", "19.9"],
            ["--tempo", "1000"],
            ["--tempo", "nan"],
        ):
            with self.subTest(argv=argv), self.assertRaises(SystemExit):
                gen_emu.parser().parse_args(["capture", *argv])
        args = gen_emu.parser().parse_args(
            ["capture", "--seed", str(2**32 - 1), "--tempo", "130.125"]
        )
        self.assertIn(".seed = 4294967295u", gen_emu.capture_header(args))
        # int(bpm * 100 + 0.5), as the render rounds: 13012.5 goes up, where Python's round() goes to even
        self.assertIn(".tempo = 13013,", gen_emu.capture_header(args))

    def test_it_reads_numbers_in_base_10_as_the_render_does(self):
        args = gen_emu.parser().parse_args(["capture", "--seed", "010"])
        self.assertIn(".seed = 10u", gen_emu.capture_header(args))
        with self.assertRaises(SystemExit):
            gen_emu.parser().parse_args(["capture", "--entropy", "0x10"])

    def test_a_capture_is_named_after_every_argument_and_never_overwrites(self):
        args = gen_emu.parser().parse_args(
            ["capture", "--seed", "7", "--entropy", "2", "--energy", "40", "--motion", "0", "--key", "C#",
             "--scale", "phrygian", "--tempo", "131.5", "--swing", "-10", "--seconds", "30"]
        )  # fmt: skip
        with tempfile.TemporaryDirectory() as tmp:
            first = gen_emu.capture_path(args, tmp)
            self.assertEqual(
                os.path.basename(first),
                "techno-acid-seed7-entropy2-energy40-motion0-keyC#-phrygian-tempo131.5-swing-10-30s.wav",
            )
            open(first, "w").close()
            second = gen_emu.capture_path(args, tmp)
            self.assertEqual(
                os.path.basename(second), os.path.basename(first)[:-4] + "-2.wav"
            )
            plain = gen_emu.capture_path(gen_emu.parser().parse_args(["capture"]), tmp)
            self.assertEqual(os.path.basename(plain), "techno-acid-seed1-60s.wav")

    def test_it_knows_the_genres_and_scales_the_firmware_has(self):
        self.assertIn("techno-acid", gen_emu.genre_names())
        scales = gen_emu.scale_names()
        self.assertEqual(scales[:4], ["major", "minor", "dorian", "phrygian"])
        self.assertIn("harmonic-minor", scales)
        self.assertEqual(len(scales), 12)

    def test_it_lists_subgenres_and_leaves_parents_out(self):
        # A parent table is never played, so no capture can name it; a subgenre is written as overrides of it.
        names = gen_emu.genre_names()
        self.assertEqual(len(names), len(set(names)))
        self.assertIn("techno-acid", names)
        self.assertIn("techno-melodic", names)
        self.assertIn("techno-dub", names)
        self.assertIn("techno-detroit", names)
        self.assertNotIn("techno", names)
        self.assertNotIn("techno-standin", names)

    def test_unknown_genre_scale_or_key_is_refused(self):
        for argv in (["--genre", "polka"], ["--scale", "blues"], ["--key", "H"]):
            with self.assertRaises(SystemExit):
                gen_emu.capture_header(gen_emu.parser().parse_args(["capture", *argv]))


class FakeEmu:
    """Records the presses a capture makes, with the virtual time each lands at."""

    def __init__(self):
        self.t, self.presses = 0, []
        self.s = self

    def wait(self, ms):
        self.t += ms * gen_emu.MS

    def button(self, name, down):
        self.presses.append((self.t, name, down))

    def pad_event(self, x, y, down):
        self.presses.append((self.t, (x, y), down))


class CaptureMutes(unittest.TestCase):
    def test_a_muted_part_is_named_and_a_folder_capture_says_so(self):
        args = gen_emu.parser().parse_args(["capture", "--mute", "3", "--mute", "1"])
        with tempfile.TemporaryDirectory() as tmp:
            path = gen_emu.capture_path(args, tmp, args.mute)
        self.assertEqual(
            os.path.basename(path), "techno-acid-seed1-mute1-mute3-60s.wav"
        )

    def test_mute_takes_the_sidebars_rows(self):
        for part in ("0", "7"):
            with self.subTest(part=part), self.assertRaises(SystemExit):
                gen_emu.parser().parse_args(["capture", "--mute", part])

    def test_each_mute_is_a_shift_press_of_the_parts_mute_pad(self):
        # MUTE/LAUNCH is x 16; part 1 is the top row, firmware y 7. SHIFT lands it on the next step, not the bar line.
        emu = FakeEmu()
        gen_emu.mute_parts(emu, [1, 3])
        held = [p for p in emu.presses if p[1] == "SHIFT"]
        pads = [p for p in emu.presses if isinstance(p[1], tuple)]
        self.assertEqual(
            [p[1:] for p in pads],
            [((16, 7), True), ((16, 7), False), ((16, 5), True), ((16, 5), False)],
        )
        self.assertEqual([p[2] for p in held], [True, False])
        self.assertLess(held[0][0], pads[0][0])
        self.assertGreater(held[1][0], pads[-1][0])

    def test_no_mutes_press_nothing(self):
        emu = FakeEmu()
        gen_emu.mute_parts(emu, [])
        self.assertEqual(emu.presses, [])

    def test_out_takes_a_wav_path_as_the_file(self):
        args = gen_emu.parser().parse_args(["capture"])
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "take.wav")
            open(path, "w").close()
            self.assertEqual(gen_emu.capture_path(args, path), path)

    def test_ab_needs_a_mute_and_names_both_captures(self):
        with self.assertRaises(SystemExit):
            gen_emu.check_capture_args(gen_emu.parser().parse_args(["capture", "--ab"]))
        with self.assertRaises(SystemExit):  # one file can't hold two captures
            gen_emu.check_capture_args(
                gen_emu.parser().parse_args(
                    ["capture", "--ab", "--mute", "2", "--out", "x.wav"]
                )
            )
        gen_emu.check_capture_args(
            gen_emu.parser().parse_args(["capture", "--ab", "--mute", "2"])
        )


def mem(when, internal, external):
    return f"1.0: gen_mem {when} internal={internal} external={external} stealable=1"


class LeakCheck(unittest.TestCase):
    def test_pairs_each_leave_with_the_entry_before_it(self):
        events = [
            mem("before_entry", 100, 50),  # cancelled: no after_leave
            "1.1: gen_entry cancelled ms=4",
            mem("before_entry", 100, 50),
            mem("after_leave", 90, 40),
        ]
        self.assertEqual(
            gen_emu.mem_cycles(events),
            [({"internal": 100, "external": 50}, {"internal": 90, "external": 40})],
        )

    def test_passes_when_memory_returns_after_the_warm_up(self):
        events = [mem("before_entry", 1000, 500), mem("after_leave", 900, 400)]
        for _ in range(3):
            events += [
                mem("before_entry", 900, 400),
                mem("after_leave", 900, 400),
            ]
        cycles = gen_emu.mem_cycles(events)
        lines, failures = gen_emu.leak_report(cycles, wanted=4)
        self.assertEqual(failures, [])
        self.assertIn("warm-up", lines[0])

    def test_any_change_on_either_heap_fails(self):
        events = [mem("before_entry", 1000, 500), mem("after_leave", 1000, 500)]
        events += [mem("before_entry", 1000, 500), mem("after_leave", 992, 500)]
        events += [mem("before_entry", 992, 500), mem("after_leave", 992, 500 - 8)]
        events += [mem("before_entry", 992, 492), mem("after_leave", 992, 492 + 8)]
        _, failures = gen_emu.leak_report(gen_emu.mem_cycles(events), wanted=4)
        self.assertEqual(len(failures), 3)
        self.assertIn("cycle 2: internal free memory changed by -8 B", failures[0])
        self.assertIn("cycle 3: external free memory changed by -8 B", failures[1])
        self.assertIn("cycle 4: external free memory changed by +8 B", failures[2])

    def test_missing_cycles_fail(self):
        events = [mem("before_entry", 1000, 500), mem("after_leave", 1000, 500)]
        _, failures = gen_emu.leak_report(gen_emu.mem_cycles(events), wanted=3)
        self.assertTrue(any("only 1 of 3" in f for f in failures))
        self.assertTrue(any("no cycle after" in f for f in failures))


class Boot(unittest.TestCase):
    def test_an_attempt_that_raises_closes_its_emulator(self):
        closed = []

        class Broken:
            def __init__(self, *args):
                pass

            def booted(self):
                raise TimeoutError("qemu stopped answering")

            def close(self):
                closed.append(True)

        with mock.patch.object(gen_emu, "Emu", Broken), self.assertRaises(TimeoutError):
            gen_emu.boot(None, "x.elf", "out", "smoke")
        self.assertEqual(closed, [True])


class LeakCycleScript(unittest.TestCase):
    def test_the_default_is_the_mem_cycle_script(self):
        args = gen_emu.parser().parse_args(["leak"])
        self.assertEqual(
            args.cycle_script, os.path.join(gen_emu.HERE, "perform_mem_cycle.txt")
        )

    def test_smoke_takes_it_too(self):
        args = gen_emu.parser().parse_args(["smoke", "--cycle-script", "reroll.txt"])
        self.assertEqual(args.cycle_script, "reroll.txt")

    def test_a_missing_one_stops_before_anything_builds(self):
        args = gen_emu.parser().parse_args(
            ["leak", "--cycle-script", "/no/such/reroll.txt"]
        )
        with self.assertRaises(SystemExit) as caught:
            gen_emu.check_args(args)
        self.assertEqual(
            str(caught.exception), "--cycle-script /no/such/reroll.txt: no such file"
        )

    def test_the_default_passes_the_check(self):
        gen_emu.check_args(gen_emu.parser().parse_args(["leak"]))

    def test_by_default_one_warm_up_cycle_and_one_checked(self):
        # #167 leaked about 1 KB every cycle, so one checked cycle finds it (#314).
        self.assertEqual(gen_emu.parser().parse_args(["leak"]).cycles, 2)
        self.assertEqual(gen_emu.WARMUP_CYCLES, 1)

    def test_fewer_cycles_than_one_past_the_warm_up_stop_before_anything_builds(self):
        with self.assertRaises(SystemExit) as caught:
            gen_emu.check_args(gen_emu.parser().parse_args(["leak", "--cycles", "1"]))
        self.assertEqual(str(caught.exception), "--cycles must be at least 2")

    def test_leak_runs_the_cycle_script_each_cycle(self):
        args = gen_emu.parser().parse_args(
            [
                "leak",
                "--cycle-script",
                "reroll.txt",
                "--cycles",
                "3",
                "--testport-elf",
                "t.elf",
            ]
        )
        read = []

        class Run:
            port = mock.Mock(events=[])

            def __init__(self, *a, **k):
                pass

            def __enter__(self):
                return self

            def __exit__(self, *exc):
                return False

        with (
            mock.patch.object(gen_emu.test_port_emu, "EmuRun", Run),
            mock.patch.object(
                gen_emu.deluge_probe, "read_script", lambda p: read.append(p) or []
            ),
            mock.patch.object(
                gen_emu.deluge_probe, "run_script", lambda *a, **k: (0, 1)
            ) as _,
            mock.patch.object(gen_emu, "log"),
        ):
            gen_emu.leak(args)
        self.assertEqual(read, ["reroll.txt"])


def bars_pcm(levels, bar_seconds=0.5):
    """S32LE stereo, one bar per entry: a 110 Hz tone at that fraction of full scale, 0 for digital silence."""
    out = bytearray()
    for level in levels:
        for i in range(int(bar_seconds * RATE)):
            v = int(level * (2**31 - 1) * math.sin(2 * math.pi * 110 * i / RATE))
            out += struct.pack("<ii", v, v)
    return bytes(out)


STEP_S = 0.5 / 16  # the tests' bars are half a second


def steps_of(*bars):
    return {b * 16 + s for b in bars for s in range(16)}


class WavCheck(unittest.TestCase):
    """gen_emu.py check (#376): is a capture whole, going by where the harness render plays."""

    def test_a_whole_capture_passes(self):
        self.assertEqual(
            gen_emu.wav_failures(
                bars_pcm([0.3] * 4), 2.0, steps_of(0, 1, 2, 3), STEP_S
            ),
            [],
        )

    def test_a_truncated_capture_fails(self):
        failures = gen_emu.wav_failures(
            bars_pcm([0.3] * 3), 2.0, steps_of(0, 1, 2), STEP_S
        )
        self.assertEqual(failures, ["1.5 s of audio, wanted 2.0 s"])

    def test_silence_where_the_harness_plays_fails(self):
        failures = gen_emu.wav_failures(
            bars_pcm([0.3, 0, 0, 0.3]), 2.0, steps_of(0, 1, 2, 3), STEP_S
        )
        self.assertEqual(
            failures,
            [
                "silent in 2 bar(s) where the harness has notes (bar 0 at PLAY): 1-2 at 0.5 s"
            ],
        )

    def test_a_silent_stretch_shorter_than_a_bar_fails(self):
        # Half a bar of silence across a bar line: no bar is silent as a whole.
        pcm = (
            bars_pcm([0.3] * 6, 0.25)[: int(0.375 * RATE) * 8]
            + bars_pcm([0], 0.25)
            + bars_pcm([0.3] * 6, 0.25)
        )
        failures = gen_emu.wav_failures(
            pcm[: int(2.0 * RATE) * 8], 2.0, steps_of(0, 1, 2, 3), STEP_S
        )
        self.assertEqual(len(failures), 1)
        self.assertTrue(failures[0].startswith("silent in "), failures)

    def test_silence_where_no_unmuted_part_plays_passes(self):
        # A drums-alone capture of a sparse Intro, or Ambient's drums before the Groove: silent on purpose.
        self.assertEqual(
            gen_emu.wav_failures(
                bars_pcm([0, 0, 0.3, 0.3]), 2.0, steps_of(2, 3), STEP_S
            ),
            [],
        )

    def test_a_sparse_part_passes_between_its_hits(self):
        hit = bars_pcm([0.3], 0.5 / 8) + bars_pcm(
            [0], 0.5 * 7 / 8
        )  # an eighth of a bar of sound, on the downbeat
        self.assertEqual(
            gen_emu.wav_failures(hit * 4, 2.0, {0, 16, 32, 48}, STEP_S), []
        )

    def test_a_step_whose_beat_runs_past_the_capture_is_not_checked(self):
        pcm = bars_pcm([0.3] * 3)[: int(1.25 * RATE) * 8]
        self.assertEqual(gen_emu.wav_failures(pcm, 1.25, steps_of(0, 1, 2), STEP_S), [])

    def test_a_run_of_full_scale_samples_fails(self):
        good = bars_pcm([0.3] * 4)
        at = int(1.2 * RATE) * 8
        bad = (
            good[:at]
            + struct.pack("<ii", 2**31 - 1, 2**31 - 1) * 200
            + good[at + 1600 :]
        )
        failures = gen_emu.wav_failures(bad, 2.0, steps_of(0, 1, 2, 3), STEP_S)
        self.assertEqual(
            failures, ["a run of 200 full-scale samples at 1.2 s (corrupt?)"]
        )

    def test_a_loud_capture_that_only_touches_full_scale_passes(self):
        self.assertEqual(
            gen_emu.wav_failures(
                bars_pcm([1.0] * 4), 2.0, steps_of(0, 1, 2, 3), STEP_S
            ),
            [],
        )


class Note:
    def __init__(self, tick, end):
        self.tick, self.end = tick, end


class Part:
    def __init__(self, *notes):
        self.notes = list(notes)


STEP = gen_emu.TICKS_PER_STEP


class ActiveSteps(unittest.TestCase):
    def test_a_step_with_a_note_in_an_unmuted_part_is_active(self):
        parts = [Part(Note(0, 6)), Part(Note(9 * STEP + 5, 9 * STEP + 20))]
        self.assertEqual(gen_emu.active_steps(parts, muted=()), {0, 9})

    def test_a_muted_rows_notes_dont_count(self):
        parts = [Part(Note(0, 6)), Part(Note(9 * STEP, 9 * STEP + 20))]
        self.assertEqual(gen_emu.active_steps(parts, muted=[2]), {0})

    def test_a_held_note_makes_each_step_it_sounds_in_active(self):
        parts = [
            Part(Note(STEP // 2, 3 * STEP))
        ]  # its note-off on step 3's line: step 3 is not held
        self.assertEqual(gen_emu.active_steps(parts, muted=()), {0, 1, 2})


class HarnessRender(unittest.TestCase):
    def test_renders_the_captures_recipe_long_enough_at_any_tempo(self):
        args = gen_emu.parser().parse_args(
            ["check", "x.wav", "--genre", "techno-dub", "--seed", "7", "--entropy", "3", "--density", "40",
             "--key", "C#", "--scale", "phrygian", "--tempo", "131.5", "--swing", "-10", "--seconds", "60",
             "--mute", "2"]
        )  # fmt: skip
        cmd = gen_emu.render_command("/r/generator_render", args, "/tmp/out")
        self.assertEqual(
            cmd,
            [
                "/r/generator_render",
                "--genre",
                "techno-dub",
                "--seed",
                "7",
                "--entropy",
                "3",
                "--density",
                "40",
                "--key",
                "C#",
                "--scale",
                "phrygian",
                "--tempo",
                "131.5",
                "--swing",
                "-10",
                "--bars",
                "251",
                "--midi-only",
                "--out",
                "/tmp/out",
            ],
        )  # fmt: skip  60 s at 999 BPM is 249.75 bars


class CaptureDump(unittest.TestCase):
    def test_each_capture_has_its_own_raw_dump(self):
        with mock.patch("os.getpid", return_value=4242):
            a = gen_emu.capture_tag("/out/techno-acid-seed1-60s.wav")
            b = gen_emu.capture_tag("/out/techno-acid-seed2-60s.wav")
        with mock.patch("os.getpid", return_value=4243):
            c = gen_emu.capture_tag("/out/techno-acid-seed1-60s.wav")
        self.assertEqual(len({a, b, c}), 3)

    def test_two_captures_of_one_recipe_into_one_folder_claim_two_wavs(self):
        args = gen_emu.parser().parse_args(["capture"])
        with tempfile.TemporaryDirectory() as tmp:
            first = gen_emu.capture_path(
                args, tmp
            )  # claimed, though nothing is written yet
            second = gen_emu.capture_path(args, tmp)
            self.assertTrue(os.path.exists(first))
        self.assertNotEqual(first, second)


class CaptureExit(unittest.TestCase):
    def run_capture(self, failures, argv=()):
        args = gen_emu.parser().parse_args(["capture", "--no-play", *argv])
        with (
            mock.patch.object(gen_emu, "warn_pin"),
            mock.patch.object(gen_emu, "build_capture", return_value="fw.elf"),
            mock.patch.object(
                gen_emu, "find_render", return_value="/r/generator_render"
            ),
            mock.patch.object(
                gen_emu, "capture_one", return_value=("x.wav", failures)
            ) as one,
        ):
            return gen_emu.capture(args), one

    def test_a_short_capture_exits_non_zero(self):
        status, _ = self.run_capture(["1.5 s of audio, wanted 2.0 s"])
        self.assertEqual(status, 1)

    def test_a_whole_capture_exits_zero(self):
        status, _ = self.run_capture([])
        self.assertEqual(status, 0)

    def test_capture_checks_by_default_and_no_check_skips_it(self):
        _, one = self.run_capture([])
        self.assertTrue(one.call_args.kwargs["check"])
        _, one = self.run_capture([], ["--no-check"])
        self.assertFalse(one.call_args.kwargs["check"])

    def test_a_capture_without_a_harness_build_stops_before_it_builds(self):
        args = gen_emu.parser().parse_args(["capture", "--no-play"])
        with (
            mock.patch.object(gen_emu, "warn_pin"),
            mock.patch.object(gen_emu, "find_render", return_value=None),
            mock.patch.object(gen_emu, "build_capture") as build,
            self.assertRaises(SystemExit),
        ):
            gen_emu.capture(args)
        build.assert_not_called()


class CheckCommand(unittest.TestCase):
    def write(self, path, data):
        gen_emu.write_wav(path, data)
        return path

    def test_check_passes_a_whole_wav_and_fails_a_truncated_one(self):
        with tempfile.TemporaryDirectory() as tmp:
            good = self.write(os.path.join(tmp, "good.wav"), bars_pcm([0.3] * 4))
            short = self.write(os.path.join(tmp, "short.wav"), bars_pcm([0.3] * 3))
            args = gen_emu.parser().parse_args(["check", good, short, "--seconds", "2"])
            with (
                mock.patch.object(gen_emu, "find_render", return_value="/r"),
                mock.patch.object(
                    gen_emu,
                    "harness_steps",
                    return_value=(steps_of(0, 1, 2, 3), STEP_S),
                ),
                mock.patch.object(gen_emu, "log") as log,
            ):
                self.assertEqual(gen_emu.check(args), 1)
            lines = [c.args[0] for c in log.call_args_list]
        self.assertTrue(
            any(line.startswith("ok ") and "good.wav" in line for line in lines), lines
        )
        self.assertTrue(
            any(line.startswith("BAD ") and "short.wav" in line for line in lines),
            lines,
        )


if __name__ == "__main__":
    unittest.main()
