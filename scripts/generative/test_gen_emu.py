"""Tests for gen_emu.py's checks and its capture build, without an emulator.

The emulator runs themselves are checked by running gen_emu.py smoke against a release build (spec §21.3).

    python3 scripts/generative/test_gen_emu.py
"""

import argparse
import math
import os
import re
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


def test_port_line_length():
    """kLineLength in test_port.cpp, the longest inject line with its terminator, as the firmware computes it."""
    src = os.path.join(gen_emu.REPO, "src", "deluge")
    with open(os.path.join(src, "io", "midi", "test_port.cpp")) as fh:
        extra = re.search(
            r"kLineLength = (\d+) \+ deluge::generative::kMaxSoundPath;", fh.read()
        )
    with open(os.path.join(src, "generative", "core", "recipe.h")) as fh:
        path = re.search(r"kMaxSoundPath = (\d+);", fh.read())
    return int(extra[1]) + int(path[1])


class CaptureRecipeLines(unittest.TestCase):
    def test_the_lines_set_genre_seed_and_each_override_given(self):
        args = gen_emu.parser().parse_args(
            [
                "capture",
                "--genre",
                "techno-acid",
                "--seed",
                "7",
                "--energy",
                "40",
                "--density",
                "0",
                "--complexity",
                "17",
                "--motion",
                "50",
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
            gen_emu.recipe_lines(args),
            [
                "next recipe genre techno-acid",
                "next recipe entropy 0",
                "next recipe seed 7",
                "next recipe macro 0 40",
                "next recipe macro 1 0",
                "next recipe macro 2 17",
                "next recipe macro 3 50",
                "next recipe tempo 13150",
                "next recipe key 1",
                "next recipe scale 3",
                "next recipe swing -10",
            ],
        )

    def test_overrides_not_given_keep_what_the_genre_rolled(self):
        args = gen_emu.parser().parse_args(["capture", "--genre", "techno-dub"])
        self.assertEqual(
            gen_emu.recipe_lines(args),
            [
                "next recipe genre techno-dub",
                "next recipe entropy 0",
                "next recipe seed 1",
            ],
        )

    def test_each_line_fits_the_test_port(self):
        # the longest genre name decides
        limit = test_port_line_length()
        args = gen_emu.parser().parse_args(["capture", "--swing", "-49"])
        for genre in gen_emu.genre_names():
            args.genre = genre
            for line in gen_emu.recipe_lines(args):
                self.assertLess(len(line), limit)

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
        lines = gen_emu.recipe_lines(args)
        self.assertIn("next recipe seed 4294967295", lines)
        # int(bpm * 100 + 0.5), as the render rounds: 13012.5 goes up, where Python's round() goes to even
        self.assertIn("next recipe tempo 13013", lines)

    def test_it_reads_numbers_in_base_10_as_the_render_does(self):
        args = gen_emu.parser().parse_args(["capture", "--seed", "010"])
        self.assertIn("next recipe seed 10", gen_emu.recipe_lines(args))
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
                gen_emu.recipe_lines(gen_emu.parser().parse_args(["capture", *argv]))


class CaptureVariants(unittest.TestCase):
    """--variant sends one "next recipe variant <set> <name>" line per flag over the test port (#548)."""

    def test_variants_go_into_the_lines_and_the_render_check_and_the_name(self):
        args = gen_emu.parser().parse_args(
            [
                "capture",
                "--genre",
                "dub",
                "--variant",
                "skank=plain",
                "--variant",
                "drums=rockers",
            ]
        )
        self.assertEqual(
            gen_emu.recipe_lines(args),
            [
                "next recipe genre dub",
                "next recipe entropy 0",
                "next recipe seed 1",
                "next recipe variant skank plain",
                "next recipe variant drums rockers",
            ],
        )
        cmd = gen_emu.render_command("/r", args, "/tmp/o", 4)
        self.assertEqual(
            cmd[cmd.index("--variant") :][:4],
            ["--variant", "skank=plain", "--variant", "drums=rockers"],
        )
        with tempfile.TemporaryDirectory() as tmp:
            self.assertIn(
                "-skank=plain-drums=rockers",
                os.path.basename(gen_emu.capture_path(args, tmp)),
            )
        for bad in ("skank", "skank=", "=plain", "Skank=plain", "skank=plain,x=y"):
            with self.subTest(bad=bad), self.assertRaises(SystemExit):
                gen_emu.parser().parse_args(["capture", "--variant", bad])
        good = gen_emu.parser().parse_args(["capture", "--variant", "accents=332"])
        self.assertEqual(good.variant, ["accents=332"])
        # No 64-byte header cap: all five Psytrance sets (or eight sets) each get their own line.
        many = gen_emu.parser().parse_args(
            ["capture", "--genre", "psytrance-classic"]
            + ["--variant", "abcdefgh=ijklmnop"] * 8
        )
        lines = gen_emu.recipe_lines(many)
        self.assertEqual(
            lines.count("next recipe variant abcdefgh ijklmnop"),
            8,
        )
        limit = test_port_line_length()
        for line in lines:
            self.assertLess(len(line), limit)


class BuildCapture(unittest.TestCase):
    def build(self, cache=None, with_variant_flag=False):
        """With with_variant_flag=True, pins that --variant builds in the same --build-dir (no -variants dir)."""
        tmp = os.path.join(self.enterContext(tempfile.TemporaryDirectory()), "b")
        os.makedirs(tmp)
        if cache is not None:
            touch(os.path.join(tmp, "CMakeCache.txt"), cache.encode())
        argv = ["capture", "--build-dir", tmp] + (
            ["--variant", "a=b"] if with_variant_flag else []
        )
        args = gen_emu.parser().parse_args(argv)
        with mock.patch.object(gen_emu, "dbt") as dbt:
            elf = gen_emu.build_capture(args)
        self.assertEqual(elf, os.path.join(tmp, "Release", "deluge.elf"))
        return [c.args[0].split()[:2] for c in dbt.call_args_list], dbt.call_args_list[
            0
        ].args[0]

    def test_a_new_build_dir_is_configured_as_a_test_port_build(self):
        for with_variant_flag in (False, True):
            with self.subTest(with_variant_flag=with_variant_flag):
                calls, first = self.build(with_variant_flag=with_variant_flag)
                self.assertEqual(calls, [["cmake", "-B"], ["cmake", "--build"]])
                self.assertIn("-DENABLE_SYSEX_TEST:BOOL=ON", first)
                self.assertIn("-UGENERATIVE_CAPTURE", first)
                self.assertNotIn("-variants", first)

    def test_a_test_port_build_dir_only_builds(self):
        for with_variant_flag in (False, True):
            with self.subTest(with_variant_flag=with_variant_flag):
                calls, _ = self.build(
                    "FOO:BOOL=OFF\nENABLE_SYSEX_TEST:BOOL=ON\n",
                    with_variant_flag=with_variant_flag,
                )
                self.assertEqual(calls, [["cmake", "--build"]])

    def test_a_build_dir_from_before_the_test_port_is_configured_again(self):
        for cache in (
            "GENERATIVE_CAPTURE_DIR:PATH=/x/generated\n",  # the Recipe compiled in (before #401)
            "GENERATIVE_CAPTURE:BOOL=ON\n",  # the Recipe patched into the ELF (before #523/#548)
            "ENABLE_SYSEX_TEST:BOOL=ON\nGENERATIVE_CAPTURE:BOOL=ON\n",
        ):
            with self.subTest(cache=cache):
                calls, first = self.build(cache)
                self.assertEqual(calls, [["cmake", "-B"], ["cmake", "--build"]])
                self.assertIn("-UGENERATIVE_CAPTURE", first)
                self.assertIn("-UGENERATIVE_CAPTURE_DIR", first)


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

    def test_a_capture_longer_than_the_render_takes_is_refused_before_the_build(self):
        too_long = ["capture", "--seconds", "5000", "--tempo", "999"]
        with self.assertRaisesRegex(SystemExit, "at most 10000"):
            gen_emu.check_capture_args(gen_emu.parser().parse_args(too_long))
        # with no --tempo, at the fastest tempo a Recipe can have
        with self.assertRaisesRegex(SystemExit, "at most 10000"):
            gen_emu.check_capture_args(gen_emu.parser().parse_args(too_long[:3]))
        gen_emu.check_capture_args(
            gen_emu.parser().parse_args(too_long[:3] + ["--tempo", "60"])
        )
        # --no-check renders nothing, so it has no bar limit
        gen_emu.check_capture_args(
            gen_emu.parser().parse_args([*too_long, "--no-check"])
        )
        with self.assertRaisesRegex(SystemExit, "fastest tempo"):
            gen_emu.check_capture_args(gen_emu.parser().parse_args(too_long[:3]))


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
            def __init__(self, *args, **kwargs):
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


def failures(*args):
    return gen_emu.wav_failures(*args).failures


class WavCheck(unittest.TestCase):
    """gen_emu.py check (#376): is a capture whole, going by where the harness render plays."""

    def test_a_whole_capture_passes(self):
        verdict = gen_emu.wav_failures(
            bars_pcm([0.3] * 4), 2.0, steps_of(0, 1, 2, 3), STEP_S
        )
        self.assertEqual(verdict, gen_emu.WavVerdict([], [], 64))

    def test_a_truncated_capture_fails(self):
        self.assertEqual(
            failures(bars_pcm([0.3] * 3), 2.0, steps_of(0, 1, 2), STEP_S),
            ["1.5 s of audio, wanted 2.0 s"],
        )

    def test_silence_where_the_harness_plays_fails(self):
        self.assertEqual(
            failures(bars_pcm([0.3, 0, 0, 0.3]), 2.0, steps_of(0, 1, 2, 3), STEP_S),
            [
                "silent in 2 bar(s) where the harness has notes (bar 0 at PLAY): 1-2 at 0.5 s"
            ],
        )

    def test_a_silent_stretch_shorter_than_a_bar_fails(self):
        # Half a bar of silence across a bar line (0.375 s to 0.625 s): no bar is silent as a whole.
        pcm = (
            bars_pcm([0.3] * 6, 0.25)[: int(0.375 * RATE) * 8]
            + bars_pcm([0], 0.25)
            + bars_pcm([0.3] * 6, 0.25)
        )
        self.assertEqual(
            failures(pcm[: int(2.0 * RATE) * 8], 2.0, steps_of(0, 1, 2, 3), STEP_S),
            [
                "silent in 2 bar(s) where the harness has notes (bar 0 at PLAY): 0-1 at 0 s"
            ],
        )

    def test_silence_where_no_unmuted_part_plays_passes(self):
        # A drums-alone capture of a sparse Intro, or Ambient's drums before the Groove: silent on purpose.
        self.assertEqual(
            failures(bars_pcm([0, 0, 0.3, 0.3]), 2.0, steps_of(2, 3), STEP_S), []
        )

    def test_a_sparse_part_passes_between_its_hits(self):
        hit = bars_pcm([0.3], 0.5 / 8) + bars_pcm(
            [0], 0.5 * 7 / 8
        )  # an eighth of a bar of sound, on the downbeat
        self.assertEqual(failures(hit * 4, 2.0, {0, 16, 32, 48}, STEP_S), [])

    def test_the_last_beat_is_checked_up_to_the_captures_end(self):
        pcm = bars_pcm([0.3] * 3)[: int(1.25 * RATE) * 8]
        verdict = gen_emu.wav_failures(pcm, 1.25, steps_of(0, 1, 2), STEP_S)
        self.assertEqual(verdict.failures, [])
        self.assertEqual(
            verdict.checked, 40
        )  # steps 0-39 start a 16th or more before 1.25 s

    def test_no_step_to_check_and_silent_throughout_fails(self):
        # An FX-alone capture whose only note, a Riser's, falls past its end.
        self.assertEqual(
            failures(bars_pcm([0] * 4), 2.0, set(), STEP_S),
            ["silent throughout, and the harness has no note to check in it"],
        )

    def test_no_step_to_check_with_sound_warns(self):
        verdict = gen_emu.wav_failures(bars_pcm([0, 0.3, 0, 0]), 2.0, set(), STEP_S)
        self.assertEqual(verdict.failures, [])
        self.assertEqual(
            verdict.warnings,
            [
                "the harness has no note to check in it: only its length and full-scale runs were checked"
            ],
        )

    def test_without_a_render_silence_is_not_checked(self):
        self.assertEqual(
            gen_emu.wav_failures(bars_pcm([0] * 4), 2.0, None, 1.0),
            gen_emu.WavVerdict([], [], 0),
        )

    def test_silence_in_a_bar_the_job_expects_quiet_passes(self):
        # An FX-alone capture: the riser starts from silence on the Build's first beat (#422).
        pcm = bars_pcm([0.3, 0, 0, 0.3])
        steps = steps_of(0, 1, 2, 3)
        self.assertEqual(
            gen_emu.wav_failures(pcm, 2.0, steps, STEP_S, quiet_bars={1, 2}).failures,
            [],
        )
        self.assertEqual(
            gen_emu.wav_failures(pcm, 2.0, steps, STEP_S, quiet_bars={1}).failures,
            [
                "silent in 1 bar(s) where the harness has notes (bar 0 at PLAY): 2 at 1 s"
            ],
        )

    def test_quiet_bars_that_leave_no_step_to_check_say_so(self):
        # #426: the harness has notes in the capture, but every one is in a quiet bar: nothing was checked.
        pcm = bars_pcm([0.3, 0, 0, 0.3])
        verdict = gen_emu.wav_failures(
            pcm, 2.0, steps_of(1, 2), STEP_S, quiet_bars={1, 2}
        )
        self.assertEqual(verdict.failures, [])
        self.assertEqual(verdict.checked, 0)
        self.assertEqual(
            verdict.warnings,
            [
                (
                    "--quiet-bars left no step to check: the harness's 32 steps in it are all in quiet bars; "
                    "only its length and full-scale runs were checked"
                )
            ],
        )
        # Silent throughout, every step quiet: still a failure, naming the quiet bars
        self.assertEqual(
            failures(bars_pcm([0] * 4), 2.0, steps_of(1, 2), STEP_S, {1, 2}),
            [
                (
                    "silent throughout, and --quiet-bars left no step to check (the harness's 32 steps in it are "
                    "all in quiet bars)"
                )
            ],
        )

    def test_quiet_bars_are_bars_and_ranges(self):
        self.assertEqual(gen_emu.bar_set("47"), frozenset({47}))
        self.assertEqual(gen_emu.bar_set("3,40-42"), frozenset({3, 40, 41, 42}))
        for bad in ("x", "5-3", "-1", ""):
            with self.assertRaises(argparse.ArgumentTypeError):
                gen_emu.bar_set(bad)

    def test_capture_and_check_take_quiet_bars(self):
        p = argparse.ArgumentParser()
        gen_emu.add_recipe_args(p)
        self.assertEqual(
            p.parse_args(["--quiet-bars", "47", "--quiet-bars", "50-51"]).quiet_bars,
            [frozenset({47}), frozenset({50, 51})],
        )
        self.assertEqual(gen_emu.quiet_bars(p.parse_args([])), frozenset())
        self.assertEqual(
            gen_emu.quiet_bars(
                p.parse_args(["--quiet-bars", "1", "--quiet-bars", "3"])
            ),
            frozenset({1, 3}),
        )

    def test_a_dropout_in_the_last_beat_fails(self):
        pcm = bars_pcm([0.3] * 3)[: int(1.125 * RATE) * 8] + bars_pcm([0], 0.125)
        self.assertEqual(
            failures(pcm, 1.25, {36}, STEP_S),
            [
                "silent in 1 bar(s) where the harness has notes (bar 0 at PLAY): 2 at 1 s"
            ],
        )

    def test_a_note_less_than_a_16th_before_the_end_is_not_checked(self):
        # Firmware latency could put its sound after the capture's end.
        pcm = bars_pcm([0.3] * 2) + bars_pcm([0], 0.02)
        verdict = gen_emu.wav_failures(pcm, 1.02, {32}, STEP_S)
        self.assertEqual((verdict.failures, verdict.checked), ([], 0))

    def test_a_long_run_of_full_scale_samples_fails(self):
        good = bars_pcm([0.3] * 4)
        at = int(1.2 * RATE) * 8
        run = gen_emu.CORRUPT_RUN + 100
        bad = (
            good[:at]
            + struct.pack("<ii", 2**31 - 1, 2**31 - 1) * run
            + good[at + run * 8 :]
        )
        self.assertEqual(
            failures(bad, 2.0, steps_of(0, 1, 2, 3), STEP_S),
            [f"a run of {run} full-scale samples at 1.2 s (clipped or corrupt)"],
        )

    def test_a_full_scale_run_ends_where_the_sign_changes(self):
        # A square wave between the two limits: never more than one sample at one limit in a row.
        top, bottom = (
            struct.pack("<ii", 2**31 - 1, 2**31 - 1),
            struct.pack("<ii", -(2**31), -(2**31)),
        )
        samples = struct.unpack(f"<{4 * 100}i", (top + bottom) * 100)
        self.assertEqual(gen_emu.longest_full_scale_run(samples)[0], 1)

    def test_a_saturated_kick_is_a_warning(self):
        # The firmware saturates its mix at the int32 limits: a 50 Hz kick at three times full scale holds INT32_MAX
        # for a few hundred samples each half cycle.
        out = bytearray()
        for i in range(int(2.0 * RATE)):
            v = 3 * (2**31 - 1) * math.sin(2 * math.pi * 50 * i / RATE)
            v = max(-(2**31), min(2**31 - 1, int(v)))
            out += struct.pack("<ii", v, v)
        verdict = gen_emu.wav_failures(bytes(out), 2.0, steps_of(0, 1, 2, 3), STEP_S)
        self.assertEqual(verdict.failures, [])
        self.assertEqual(len(verdict.warnings), 1)
        self.assertRegex(
            verdict.warnings[0],
            r"^a run of \d{3} full-scale samples at 0\.0 s: the mix clips$",
        )

    def test_a_loud_capture_that_only_touches_full_scale_passes(self):
        self.assertEqual(
            gen_emu.wav_failures(
                bars_pcm([1.0] * 4), 2.0, steps_of(0, 1, 2, 3), STEP_S
            ),
            gen_emu.WavVerdict([], [], 64),
        )


class Note:
    def __init__(self, tick, end, key=60):
        self.tick, self.end, self.key = tick, end, key


class Part:
    def __init__(self, *notes, lane_notes=None):
        self.notes = list(notes)
        self.lane_notes = lane_notes or {}


STEP = gen_emu.TICKS_PER_STEP


class ActiveSteps(unittest.TestCase):
    def test_a_step_with_a_note_in_an_unmuted_part_is_active(self):
        parts = [Part(Note(0, 6)), Part(Note(9 * STEP + 5, 9 * STEP + 20))]
        self.assertEqual(gen_emu.active_steps(parts, muted=()), {0, 9})

    def test_a_muted_rows_notes_dont_count(self):
        parts = [Part(Note(0, 6)), Part(Note(9 * STEP, 9 * STEP + 20))]
        self.assertEqual(gen_emu.active_steps(parts, muted=[2]), {0})

    def test_only_a_held_notes_onset_counts(self):
        # A short-decay sound under a long note is silent before its note-off.
        parts = [Part(Note(STEP // 2, 12 * STEP))]
        self.assertEqual(gen_emu.active_steps(parts, muted=()), {0})

    def test_a_riser_counts_on_the_beat_before_the_drop_not_its_onset(self):
        # The firmware starts a sample Riser late, so it ends on the Drop (riser_timing.h); the render's note starts
        # with the Build and ends on the Drop. A Fall on the Riser's onset step still counts there.
        fx = Part(
            Note(16 * STEP, 32 * STEP, key=84),
            Note(16 * STEP, 17 * STEP, key=85),
            Note(20 * STEP, 21 * STEP, key=85),
            lane_notes=RISER_FALL,
        )
        self.assertEqual(gen_emu.active_steps([fx], muted=()), {16, 20, 28})

    def test_a_riser_shorter_than_a_beat_counts_on_its_onset(self):
        fx = Part(Note(30 * STEP, 32 * STEP, key=84), lane_notes=RISER_FALL)
        self.assertEqual(gen_emu.active_steps([fx], muted=()), {30})

    def test_a_riser_whose_drop_is_past_the_render_doesnt_count(self):
        fx = Part(Note(16 * STEP, None, key=84), lane_notes=RISER_FALL)
        self.assertEqual(gen_emu.active_steps([fx], muted=()), set())


RISER_FALL = {84: "Riser", 85: "Fall"}


class Piece:
    def __init__(self, tempo, parts=()):
        self.recipe = {"tempo": str(tempo)}
        self.parts = list(parts)


class HarnessRender(unittest.TestCase):
    def test_renders_the_captures_recipe_flags(self):
        args = gen_emu.parser().parse_args(
            ["check", "x.wav", "--genre", "techno-dub", "--seed", "7", "--entropy", "3", "--density", "40",
             "--key", "C#", "--scale", "phrygian", "--tempo", "131.5", "--swing", "-10", "--seconds", "60",
             "--mute", "2"]
        )  # fmt: skip
        cmd = gen_emu.render_command("/r/generator_render", args, "/tmp/out", 34)
        self.assertEqual(
            cmd,
            ["/r/generator_render", "--genre", "techno-dub", "--seed", "7", "--entropy", "3", "--density", "40",
             "--key", "C#", "--scale", "phrygian", "--tempo", "131.5", "--swing", "-10", "--bars", "34",
             "--midi-only", "--out", "/tmp/out"],
        )  # fmt: skip

    def test_it_renders_one_bar_past_the_seconds_at_the_tempo(self):
        self.assertEqual(gen_emu.bars_for(60, 131.5), 34)  # 60 s is 32.9 bars
        self.assertEqual(gen_emu.bars_for(60, 120), 31)

    def test_more_bars_than_the_render_takes_are_refused(self):
        with self.assertRaisesRegex(ValueError, "at most 10000"):
            gen_emu.bars_for(5000, 999)

    def test_a_rolled_tempo_is_read_from_a_one_bar_render_first(self):
        args = gen_emu.parser().parse_args(["check", "x.wav", "--seconds", "60"])
        with mock.patch.object(
            gen_emu, "render_piece", side_effect=[Piece(120), Piece(120, ["p"])]
        ) as render:
            parts, step_seconds = gen_emu.harness_piece("/r", args)
        self.assertEqual([c.args[2] for c in render.call_args_list], [1, 31])
        self.assertEqual((parts, step_seconds), (["p"], 2.0 / 16))

    def test_a_given_tempo_renders_once(self):
        args = gen_emu.parser().parse_args(
            ["check", "x.wav", "--seconds", "60", "--tempo", "120"]
        )
        with mock.patch.object(
            gen_emu, "render_piece", return_value=Piece(120)
        ) as render:
            gen_emu.harness_piece("/r", args)
        self.assertEqual([c.args[2] for c in render.call_args_list], [31])


def every_flag_args():
    longest = max(gen_emu.genre_names(), key=len)
    return gen_emu.parser().parse_args(
        ["capture", "--genre", longest, "--seed", str(2**32 - 1), "--entropy", str(2**32 - 1), "--energy", "50",
         "--density", "50", "--complexity", "50", "--motion", "50", "--key", "C#", "--scale", "harmonic-minor",
         "--tempo", "137.25", "--swing", "-49", "--seconds", "170"]
    )  # fmt: skip


class CaptureDump(unittest.TestCase):
    def test_each_capture_has_its_own_raw_dump(self):
        with mock.patch("os.getpid", return_value=4242):
            a = gen_emu.capture_tag("/out/techno-acid-seed1-60s.wav")
            b = gen_emu.capture_tag("/out/techno-acid-seed2-60s.wav")
        with mock.patch("os.getpid", return_value=4243):
            c = gen_emu.capture_tag("/out/techno-acid-seed1-60s.wav")
        self.assertEqual(len({a, b, c}), 3)

    def test_the_qmp_socket_path_is_short_for_a_capture_with_every_flag(self):
        args = every_flag_args()
        with (
            tempfile.TemporaryDirectory() as tmp,
            mock.patch("os.getpid", return_value=4194304),
        ):
            wav = gen_emu.capture_path(args, tmp, mutes=[1, 2, 3])
            names = [gen_emu.boot_name(gen_emu.capture_tag(wav), n) for n in (1, 2)]
            socks = [gen_emu.qmp_socket(name) for name in names]
        self.assertGreater(len(names[0]), 100)  # the stem alone would be too long
        self.assertLess(max(len(s.encode()) for s in socks), 100)
        self.assertNotEqual(socks[0], socks[1])  # each boot attempt its own

    def test_two_captures_of_one_recipe_into_one_folder_claim_two_wavs(self):
        args = gen_emu.parser().parse_args(["capture"])
        with tempfile.TemporaryDirectory() as tmp:
            first = gen_emu.capture_path(
                args, tmp
            )  # claimed, though nothing is written yet
            second = gen_emu.capture_path(args, tmp)
            self.assertTrue(os.path.exists(first))
        self.assertNotEqual(first, second)


def touch(path, data=b""):
    with open(path, "wb") as fh:
        fh.write(data)


DEAD_PID = 2**22 + 1  # above macOS's and Linux's default pid_max


class BootFiles(unittest.TestCase):
    def test_a_captures_files_are_removed_by_extension_and_no_one_elses(self):
        with tempfile.TemporaryDirectory() as tmp:
            for name in ("a-pid1-boot1.s32", "a-pid1-boot2.s32", "a-pid1-boot1.log", "a-pid2-boot1.s32",
                         "a-pid1.wav"):  # fmt: skip
                touch(os.path.join(tmp, name))
            gen_emu.remove_boot_files(tmp, "a-pid1", "s32")
            left = sorted(os.listdir(tmp))
        self.assertEqual(left, ["a-pid1-boot1.log", "a-pid1.wav", "a-pid2-boot1.s32"])

    def test_a_killed_captures_dumps_and_empty_wav_are_removed(self):
        live = os.getpid()
        with tempfile.TemporaryDirectory() as tmp:
            for name, data in (
                (f"dead-pid{DEAD_PID}-boot1.s32", b"x"),
                (f"dead-pid{DEAD_PID}-boot1.log", b"x"),
                ("dead.wav", b""),
                (f"done-pid{DEAD_PID}-boot1.s32", b"x"),
                ("done.wav", b"RIFF"),  # whole: written before the dump was left
                (f"live-pid{live}-boot1.s32", b"x"),
                ("live.wav", b""),
            ):
                touch(os.path.join(tmp, name), data)
            with mock.patch.object(gen_emu, "log"):
                gen_emu.clear_dead_captures(tmp)
            left = sorted(os.listdir(tmp))
        self.assertEqual(
            left,
            [f"dead-pid{DEAD_PID}-boot1.log", "done.wav", f"live-pid{live}-boot1.s32", "live.wav"],
        )  # fmt: skip


class CapturePlays(unittest.TestCase):
    def test_a_capture_is_silent_unless_asked_to_play(self):
        self.assertFalse(gen_emu.parser().parse_args(["capture"]).play)
        self.assertFalse(gen_emu.parser().parse_args(["capture", "--no-play"]).play)
        self.assertTrue(gen_emu.parser().parse_args(["capture", "--play"]).play)


class CaptureExit(unittest.TestCase):
    def run_capture(self, failures, argv=()):
        args = gen_emu.parser().parse_args(["capture", "--no-play", *argv])
        calls = []
        with (
            mock.patch.object(gen_emu, "warn_pin"),
            mock.patch.object(
                gen_emu,
                "build_capture",
                side_effect=lambda a: calls.append("firmware") or "fw.elf",
            ),
            mock.patch.object(
                gen_emu,
                "fresh_harness_piece",
                side_effect=lambda a: calls.append("render") or (["parts"], 0.1),
            ),
            mock.patch.object(
                gen_emu, "capture_one", return_value=("x.wav", failures)
            ) as one,
        ):
            return gen_emu.capture(args), one, calls

    def test_a_short_capture_exits_non_zero(self):
        status, _, _ = self.run_capture(["1.5 s of audio, wanted 2.0 s"])
        self.assertEqual(status, 1)

    def test_a_whole_capture_exits_zero(self):
        status, _, _ = self.run_capture([])
        self.assertEqual(status, 0)

    def test_capture_builds_the_render_after_the_firmware_and_checks_with_it(self):
        _, one, calls = self.run_capture([])
        self.assertEqual(calls, ["firmware", "render"])
        self.assertEqual(one.call_args.args[3], (["parts"], 0.1))

    def test_no_check_builds_no_render_and_checks_nothing(self):
        _, one, calls = self.run_capture([], ["--no-check"])
        self.assertEqual(calls, ["firmware"])
        self.assertIsNone(one.call_args.args[3])

    def test_ab_renders_once_for_both_captures(self):
        import wav_levels

        with mock.patch.object(wav_levels, "main", return_value=0):
            _, one, calls = self.run_capture([], ["--ab", "--mute", "2"])
        self.assertEqual(calls, ["firmware", "render"])
        self.assertEqual(
            [(c.args[2], c.args[3]) for c in one.call_args_list],
            [([], (["parts"], 0.1)), ([2], (["parts"], 0.1))],
        )

    def test_elf_builds_no_firmware_and_captures_with_that_elf(self):
        with mock.patch.object(gen_emu.os.path, "isfile", return_value=True):
            _, one, calls = self.run_capture([], ["--elf", "built.elf"])
        self.assertEqual(calls, ["render"])
        self.assertEqual(one.call_args.args[1], "built.elf")

    def test_a_missing_elf_stops_before_anything_runs(self):
        args = gen_emu.parser().parse_args(
            ["capture", "--no-play", "--elf", "no/such.elf"]
        )
        with (
            mock.patch.object(gen_emu, "warn_pin"),
            mock.patch.object(gen_emu, "capture_one") as one,
            self.assertRaises(SystemExit),
        ):
            gen_emu.capture(args)
        one.assert_not_called()

    def test_an_unknown_genre_stops_before_the_build(self):
        args = gen_emu.parser().parse_args(["capture", "--no-play", "--genre", "polka"])
        with (
            mock.patch.object(gen_emu, "warn_pin"),
            mock.patch.object(gen_emu, "build_capture") as build,
            self.assertRaises(SystemExit),
        ):
            gen_emu.capture(args)
        build.assert_not_called()

    def test_a_render_that_isnt_a_program_stops_before_the_build(self):
        args = gen_emu.parser().parse_args(
            ["capture", "--no-play", "--render", "/no/such/generator_render"]
        )
        with (
            mock.patch.object(gen_emu, "warn_pin"),
            mock.patch.object(gen_emu, "build_capture") as build,
            self.assertRaises(SystemExit),
        ):
            gen_emu.capture(args)
        build.assert_not_called()


class EndPiece:
    """A theory_review Piece as end_press reads it: tempo, Section markers, bars played."""

    def __init__(self, tempo, sections=(), bars=0):
        self.recipe = {"tempo": str(tempo)}
        self.sections = list(sections)
        self.bars = bars


# Two Arc cycles at 120 BPM (2 s a bar): Drops at bars 40 and 96; with End on the second, the Outro ends at bar 112.
TWO_DROPS = [(0, "INTRO 1"), (8, "GROOVE 1"), (24, "BREAK 1"), (32, "BUILD 1"), (40, "GROOVE 2"),
             (64, "BREAK 2"), (88, "BUILD 2"), (96, "GROOVE 3"), (104, "OUTRO 1")]  # fmt: skip


class End(unittest.TestCase):
    """#489: capture and check press End at a bar, or from the Nth Drop, as generator_render arms it."""

    def args(self, *argv):
        return gen_emu.parser().parse_args(["capture", *argv])

    def test_end_after_drop_takes_an_optional_count_and_end_at_bar_a_bar(self):
        self.assertIsNone(self.args().end_after_drop)
        self.assertIsNone(self.args().end_at_bar)
        self.assertEqual(self.args("--end-after-drop").end_after_drop, 1)
        self.assertEqual(self.args("--end-after-drop", "2").end_after_drop, 2)
        self.assertEqual(self.args("--end-at-bar", "40").end_at_bar, 40)
        for bad in (
            ["--end-after-drop", "0"],
            ["--end-at-bar", "-1"],
            ["--end-at-bar", "x"],
        ):
            with self.subTest(bad=bad), self.assertRaises(SystemExit):
                self.args(*bad)

    def test_both_are_refused_before_anything_builds(self):
        with self.assertRaisesRegex(SystemExit, "give one"):
            gen_emu.check_args(self.args("--end-after-drop", "--end-at-bar", "4"))

    def test_the_harness_render_arms_end_the_same_way(self):
        for argv, flags in (
            (["--end-after-drop"], ["--end-after-drop", "1"]),
            (["--end-after-drop", "2"], ["--end-after-drop", "2"]),
            (["--end-at-bar", "40"], ["--end-at-bar", "40"]),
            ([], []),
        ):
            with self.subTest(argv=argv):
                cmd = gen_emu.render_command("/r", self.args(*argv), "/tmp/o", 9)
                self.assertEqual(
                    cmd[-5:], ["--bars", "9", "--midi-only", "--out", "/tmp/o"]
                )
                self.assertEqual(
                    cmd[cmd.index("--seed") + 2 : cmd.index("--bars")], flags
                )

    def test_a_drop_is_a_groove_a_build_lands_on(self):
        self.assertEqual(gen_emu.drop_bars(TWO_DROPS), [40, 96])
        self.assertEqual(gen_emu.drop_bars([(0, "INTRO 1"), (8, "GROOVE 1")]), [])

    def test_the_capture_is_named_after_its_end(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertTrue(
                gen_emu.capture_path(self.args("--end-after-drop", "2"), tmp).endswith(
                    "-end-drop2-60s.wav"
                )
            )
            self.assertTrue(
                gen_emu.capture_path(self.args("--end-at-bar", "40"), tmp).endswith(
                    "-end-bar40-60s.wav"
                )
            )

    def end_press(self, *argv, sections=TWO_DROPS, bars=112):
        args = self.args("--seconds", "240", "--tempo", "120", *argv)
        with mock.patch.object(
            gen_emu, "render_piece", return_value=EndPiece(120, sections, bars)
        ):
            return gen_emu.end_press("/r", args)

    def test_end_after_the_nth_drop_holds_end_from_its_first_bar(self):
        self.assertEqual(
            self.end_press("--end-after-drop", "2"), (96, 192.0, 112, 224.0)
        )
        self.assertEqual(
            self.end_press("--end-at-bar", "50", bars=121)[:2], (50, 100.0)
        )

    def test_a_piece_still_playing_at_the_end_of_seconds_says_so(self):
        press = self.end_press("--end-at-bar", "50", sections=TWO_DROPS[:5], bars=121)
        self.assertEqual((press.ends_bar, press.ends_seconds), (None, None))

    def test_too_few_drops_or_a_bar_past_seconds_is_refused(self):
        with self.assertRaisesRegex(ValueError, "1 Drop"):
            self.end_press("--end-after-drop", "2", sections=TWO_DROPS[:5])
        with self.assertRaisesRegex(ValueError, "past --seconds"):
            self.end_press("--end-at-bar", "120")

    def test_no_end_renders_nothing(self):
        with mock.patch.object(gen_emu, "render_piece") as render:
            self.assertIsNone(gen_emu.end_press("/r", self.args()))
        render.assert_not_called()

    def test_end_is_a_hold_of_the_end_pad_past_the_arm_time(self):
        emu = FakeEmu()
        gen_emu.press_end(emu)
        (down, pad, d), (up, pad2, u) = emu.presses
        self.assertEqual((pad, d, pad2, u), ((17, 0), True, (17, 0), False))
        self.assertGreater(up - down, 600 * gen_emu.MS)

    def test_capture_passes_the_end_to_each_capture(self):
        args = self.args("--no-play", "--end-at-bar", "8")
        press = gen_emu.EndPress(8, 16.0, 24, 48.0)
        with (
            mock.patch.object(gen_emu, "warn_pin"),
            mock.patch.object(gen_emu, "build_capture", return_value="fw.elf"),
            mock.patch.object(
                gen_emu, "fresh_harness_piece", return_value=(["p"], 0.1)
            ),
            mock.patch.object(gen_emu, "end_press", return_value=press) as end,
            mock.patch.object(
                gen_emu, "capture_one", return_value=("x.wav", [])
            ) as one,
            mock.patch.object(gen_emu, "log"),
        ):
            self.assertEqual(gen_emu.capture(args), 0)
        self.assertEqual(end.call_args.args[0], gen_emu.HARNESS_RENDER)
        self.assertEqual(one.call_args.args[4], press)

    def test_no_check_with_end_still_needs_a_render(self):
        args = self.args("--no-play", "--no-check", "--end-after-drop")
        with (
            mock.patch.object(gen_emu, "warn_pin"),
            mock.patch.object(gen_emu, "find_render", return_value=None),
            mock.patch.object(gen_emu, "build_capture") as build,
            self.assertRaisesRegex(SystemExit, "End needs generator_render"),
        ):
            gen_emu.capture(args)
        build.assert_not_called()


class DumpEmu:
    """An emulator whose audio dump lags its virtual clock by `lag_ms`."""

    def __init__(self, lag_ms):
        self.t, self.lag = 0, lag_ms * gen_emu.MS

    def run_to(self, ns, each_step=None):
        self.t = ns

    def dump_size(self):
        return max(0, (self.t - self.lag) * RATE // SEC) * gen_emu.FRAME


class AudioTail(unittest.TestCase):
    """#489: the dump lags the virtual clock, so a capture runs on until it holds the whole --seconds."""

    def test_it_runs_on_until_the_dump_holds_the_seconds(self):
        emu = DumpEmu(lag_ms=37)
        emu.t = 10 * SEC  # --seconds 10 from bar 0 at t 0, now run
        ran = gen_emu.run_for_audio(emu, 0, 10)
        self.assertGreaterEqual(emu.dump_size(), int(10 * RATE) * gen_emu.FRAME)
        self.assertEqual(ran, 40)  # in 10 ms steps

    def test_a_whole_dump_runs_no_further(self):
        emu = DumpEmu(lag_ms=0)
        emu.t = 10 * SEC
        self.assertEqual(gen_emu.run_for_audio(emu, 0, 10), 0)

    def test_it_gives_up_after_the_tail_limit(self):
        emu = DumpEmu(lag_ms=5000)
        emu.t = 10 * SEC
        self.assertEqual(gen_emu.run_for_audio(emu, 0, 10), gen_emu.TAIL_LIMIT_S * 1000)


class RecipeOverTestPort(unittest.TestCase):
    def port(self, replies):
        """A TestPort whose link records what it sends and answers each look from `replies` (seq filled in)."""
        port = gen_emu.deluge_probe.TestPort(mock.Mock(), on_hardware=False)
        self.sent = []
        port.link.send = self.sent.append
        answers = iter(replies)

        def receive_until(match, timeout):
            status = next(answers)
            if status is None:
                return None
            msg = gen_emu.deluge_probe.HDR + [
                0x02,
                gen_emu.deluge_probe.STATUS_REPLY,
                port.seq,
                status,
                0xF7,
            ]
            return msg if match(msg) else None

        port.receive_until = receive_until
        return port

    def test_each_line_runs_the_vm_in_fixed_steps_until_its_reply(self):
        emu = FakeEmu()
        emu.port = self.port([None, None, 0, 0])
        gen_emu.send_recipe(emu, ["next recipe seed 1", "next recipe entropy 0"])
        self.assertEqual(
            emu.t, 4 * gen_emu.LINK_STEP_MS * gen_emu.MS
        )  # 3 for the first, 1 for the second
        lines = [bytes(m[8:-1]).decode() for m in self.sent[1:]]  # after the subscribe
        self.assertEqual(lines, ["next recipe seed 1", "next recipe entropy 0"])

    def test_a_reply_that_took_longer_than_usual_warns(self):
        emu = FakeEmu()
        emu.port = self.port([None, 0])
        with mock.patch.object(gen_emu, "log") as log:
            gen_emu.send_recipe(emu, ["next recipe seed 1"])
        self.assertIn("LINK_DELIVER_S", log.call_args.args[0])

    def test_no_reply_to_the_first_line_asks_whether_it_is_a_test_port_build(self):
        emu = FakeEmu()
        emu.port = self.port([None] * (gen_emu.REPLY_LIMIT_MS // gen_emu.LINK_STEP_MS))
        with self.assertRaisesRegex(RuntimeError, "test-port build"):
            gen_emu.send_recipe(emu, ["next recipe genre techno-acid"])

    def test_a_refused_or_unanswered_line_fails_the_capture(self):
        emu = FakeEmu()
        emu.port = self.port([gen_emu.deluge_probe.PARSE_ERROR])
        with self.assertRaises(RuntimeError):
            gen_emu.send_recipe(emu, ["next recipe tempo 5"])
        emu.port = self.port([None] * (gen_emu.REPLY_LIMIT_MS // gen_emu.LINK_STEP_MS))
        with self.assertRaises(RuntimeError):
            gen_emu.send_recipe(emu, ["next recipe seed 1"])

    def test_an_entry_that_never_took_the_recipe_fails_the_capture(self):
        port = mock.Mock(
            events=["8.69: gen_perform_entered techno-acid"]
        )  # pump() reads none
        with self.assertRaises(RuntimeError):
            gen_emu.check_recipe_taken(mock.Mock(port=port))
        port.subscribe.assert_called_with(False)  # no test events while it plays
        port.events.insert(0, "8.09: gen_next_recipe taken")
        gen_emu.check_recipe_taken(mock.Mock(port=port))

    def test_an_exchange_ends_at_a_fixed_virtual_time_or_fails(self):
        emu = FakeEmu()
        emu.run_to = lambda ns: setattr(emu, "t", max(emu.t, ns))
        emu.t = 1 * gen_emu.SEC
        gen_emu.settle(emu, 3 * gen_emu.SEC, "x")
        self.assertEqual(emu.t, 3 * gen_emu.SEC)
        emu.t = 4 * gen_emu.SEC
        with self.assertRaises(RuntimeError):
            gen_emu.settle(emu, 3 * gen_emu.SEC, "x")

    def test_the_link_gives_qemu_time_to_hand_over_the_bytes(self):
        link = gen_emu.IcountLink()
        self.addCleanup(link.close)
        with (
            mock.patch.object(gen_emu.deluge_probe.UdpLink, "send") as send,
            mock.patch.object(gen_emu.time, "sleep") as sleep,
        ):
            link.send([0xF0, 0xF7])
        send.assert_called_once_with([0xF0, 0xF7])
        sleep.assert_called_once_with(gen_emu.LINK_DELIVER_S)


class CaptureOne(unittest.TestCase):
    """capture_one around a faked emulator: what it leaves on disk and what it logs."""

    def run_one(self, pcm, harness, argv=()):
        self.sent = []
        tmp = self.enterContext(tempfile.TemporaryDirectory())
        args = gen_emu.parser().parse_args(
            ["capture", "--out", tmp, "--seconds", "2", *argv]
        )

        def boot(args, elf, out, tag, test_port=False):
            self.booted = (elf, tag, test_port)
            for n in (1, 2):
                touch(os.path.join(out, gen_emu.boot_name(tag, n) + ".s32"))
                touch(os.path.join(out, gen_emu.boot_name(tag, n) + ".log"))
            return mock.Mock(t=0)

        with (
            mock.patch.object(gen_emu, "boot", side_effect=boot),
            mock.patch.object(
                gen_emu,
                "send_recipe",
                side_effect=lambda emu, lines: self.sent.append(lines),
            ),
            mock.patch.object(gen_emu, "check_recipe_taken") as taken,
            mock.patch.object(gen_emu, "settle"),
            mock.patch.object(gen_emu, "enter_perform", return_value=True),
            mock.patch.object(gen_emu, "check_perform_plays", return_value=pcm),
            mock.patch.object(gen_emu, "log") as log,
        ):
            wav, fails = gen_emu.capture_one(args, "fw.elf", [], harness)
        self.taken = taken.call_count
        return tmp, wav, fails, [c.args[0] for c in log.call_args_list]

    def test_a_whole_capture_leaves_only_its_wav(self):
        parts = [Part(*(Note(s * STEP, s * STEP + 6) for s in range(64)))]
        tmp, wav, fails, _ = self.run_one(bars_pcm([0.3] * 4), (parts, STEP_S))
        self.assertEqual(fails, [])
        self.assertEqual(os.listdir(tmp), [os.path.basename(wav)])

    def test_it_boots_the_build_with_the_test_port_and_sends_its_recipe(self):
        parts = [Part(*(Note(s * STEP, s * STEP + 6) for s in range(64)))]
        for argv in (
            ["--seed", "5"],
            ["--seed", "5", "--variant", "build=long"],
        ):
            with self.subTest(argv=argv):
                _, wav, _, _ = self.run_one(bars_pcm([0.3] * 4), (parts, STEP_S), argv)
                self.assertEqual(
                    self.booted, ("fw.elf", gen_emu.capture_tag(wav), True)
                )
                args = gen_emu.parser().parse_args(["capture", *argv])
                self.assertEqual(self.sent, [gen_emu.recipe_lines(args)])
                self.assertEqual(self.taken, 1)

    def test_a_bad_capture_keeps_its_logs(self):
        parts = [Part(Note(0, 6))]
        tmp, wav, fails, _ = self.run_one(bars_pcm([0] * 4), (parts, STEP_S))
        self.assertEqual(len(fails), 1)
        stem = os.path.splitext(os.path.basename(wav))[0]
        self.assertEqual(
            sorted(os.listdir(tmp)),
            [f"{stem}-pid{os.getpid()}-boot{n}.log" for n in (1, 2)] + [stem + ".wav"],
        )

    def test_a_capture_with_a_warning_keeps_its_logs(self):
        tmp, wav, fails, _ = self.run_one(bars_pcm([0] * 4), None)
        self.assertEqual(fails, [])
        stem = os.path.splitext(os.path.basename(wav))[0]
        self.assertIn(f"{stem}-pid{os.getpid()}-boot1.log", os.listdir(tmp))

    def test_unchecked_silence_is_a_warning(self):
        _, _, fails, lines = self.run_one(bars_pcm([0] * 4), None)
        self.assertEqual(fails, [])
        self.assertIn("WARNING silent from 0.0 s to 2.0 s", lines)

    def test_a_capture_that_raises_removes_its_claimed_wav(self):
        for variant in ([], ["--variant", "build=long"]):
            with self.subTest(variant=variant):
                tmp = self.enterContext(tempfile.TemporaryDirectory())
                out = os.path.join(tmp, "out")
                args = gen_emu.parser().parse_args(["capture", "--out", out, *variant])
                with (
                    mock.patch.object(
                        gen_emu, "boot", side_effect=RuntimeError("no boot")
                    ),
                    self.assertRaises(RuntimeError),
                ):
                    gen_emu.capture_one(args, "fw.elf", [], None)
                self.assertEqual(os.listdir(out), [])


class CheckCommand(unittest.TestCase):
    def write(self, path, data):
        gen_emu.write_wav(path, data)
        return path

    def test_check_passes_a_whole_wav_and_fails_a_truncated_one(self):
        parts = [Part(*(Note(s * STEP, s * STEP + 6) for s in range(64)))]
        with tempfile.TemporaryDirectory() as tmp:
            good = self.write(os.path.join(tmp, "good.wav"), bars_pcm([0.3] * 4))
            short = self.write(os.path.join(tmp, "short.wav"), bars_pcm([0.3] * 3))
            args = gen_emu.parser().parse_args(["check", good, short, "--seconds", "2"])
            with (
                mock.patch.object(gen_emu, "find_render", return_value="/r"),
                mock.patch.object(gen_emu, "require_render"),
                mock.patch.object(gen_emu, "log_render"),
                mock.patch.object(
                    gen_emu, "harness_piece", return_value=(parts, STEP_S)
                ),
                mock.patch.object(gen_emu, "log") as log,
            ):
                self.assertEqual(gen_emu.check_wavs(args), 1)
            lines = [c.args[0] for c in log.call_args_list]
        self.assertIn(f"ok {good} (2 s, 64 sounding steps checked)", lines)
        self.assertIn(f"BAD {short}: 1.5 s of audio, wanted 2.0 s", lines)

    def test_check_fails_a_silent_capture_of_a_part_whose_only_lane_is_the_riser(self):
        # The Riser's Drop is past the capture's end: no step to check, so silence throughout fails it.
        parts = [Part(Note(48 * STEP, 96 * STEP, key=84), lane_notes={84: "Riser"})]
        with tempfile.TemporaryDirectory() as tmp:
            silent = self.write(os.path.join(tmp, "silent.wav"), bars_pcm([0] * 4))
            args = gen_emu.parser().parse_args(["check", silent, "--seconds", "2"])
            with (
                mock.patch.object(gen_emu, "find_render", return_value="/r"),
                mock.patch.object(gen_emu, "require_render"),
                mock.patch.object(gen_emu, "log_render"),
                mock.patch.object(
                    gen_emu, "harness_piece", return_value=(parts, STEP_S)
                ),
                mock.patch.object(gen_emu, "log") as log,
            ):
                self.assertEqual(gen_emu.check_wavs(args), 1)
            lines = [c.args[0] for c in log.call_args_list]
        self.assertIn(
            f"BAD {silent}: silent throughout, and the harness has no note to check in it",
            lines,
        )


class RenderAge(unittest.TestCase):
    def test_a_render_older_than_the_generators_source_warns(self):
        with tempfile.NamedTemporaryFile() as fh:
            os.utime(fh.name, (0, 0))
            with mock.patch.object(gen_emu, "log") as log:
                gen_emu.log_render(fh.name)
        lines = [c.args[0] for c in log.call_args_list]
        self.assertTrue(
            lines[0].startswith(f"harness render: {fh.name} (built "), lines
        )
        self.assertTrue(lines[1].startswith("WARNING src/deluge/generative/") or
                        lines[1].startswith("WARNING tests/spec/generator/"), lines)  # fmt: skip

    def test_the_sources_are_what_generator_render_builds_from(self):
        sources = [
            os.path.relpath(f, gen_emu.REPO) for f in gen_emu.generator_source_files()
        ]
        self.assertIn("src/deluge/generative/core/generator.cpp", sources)
        self.assertIn("tests/spec/generator/render/command.cpp", sources)
        self.assertIn("tests/spec/generator/harness.h", sources)
        self.assertIn("tests/spec/generator/CMakeLists.txt", sources)
        self.assertFalse([f for f in sources if f.endswith("_spec.cpp")])

    def test_a_fresh_render_does_not(self):
        with (
            tempfile.NamedTemporaryFile() as fh,
            mock.patch.object(gen_emu, "log") as log,
        ):
            gen_emu.log_render(fh.name)
        self.assertEqual(len(log.call_args_list), 1)


class GoldenScript(unittest.TestCase):
    def test_the_script_picks_a_genre_that_has_a_golden(self):
        with open(gen_emu.GOLDEN_SCRIPT) as fh:
            genre = gen_emu.golden_genre(fh.read())
        self.assertTrue(os.path.isfile(gen_emu.golden_song.golden_path(genre)))

    def test_the_last_pick_genre_line_names_it(self):
        text = "start_state\npick genre techno-acid\nfoo\npick genre techno-dub\n"
        self.assertEqual(gen_emu.golden_genre(text), "techno-dub")
        with self.assertRaises(ValueError):
            gen_emu.golden_genre("start_state\n")

    def test_the_script_saves_the_song_the_check_reads(self):
        with open(gen_emu.GOLDEN_SCRIPT) as fh:
            text = fh.read()
        name = os.path.splitext(os.path.basename(gen_emu.GOLDEN_SONG))[0]
        self.assertIn(f"\nsave song {name}\n", text)
        self.assertIn(f"\nload song {gen_emu.GOLDEN_SONG}\n", text)


class SmokeCapture(unittest.TestCase):
    def test_new_song_files_returns_only_songs_added_on_the_clone(self):
        class FakeFat:
            def __init__(self, image):
                self.image = image

            def __enter__(self):
                return self

            def __exit__(self, *exc):
                return False

            def exists(self, path):
                return path == gen_emu.SONG_FOLDER

            def listdir(self, path):
                if self.image == "master.img":
                    return ["SONG001.XML"]
                return ["SONG001.XML", "GEN ACID TECHNO 1A2B3C4 LIVE.XML"]

        with mock.patch.object(gen_emu.fat_image, "FatImage", FakeFat):
            self.assertEqual(
                gen_emu.new_song_files("clone.img", "master.img"),
                ["GEN ACID TECHNO 1A2B3C4 LIVE.XML"],
            )

    def test_clone_card_refuses_to_overwrite_the_master(self):
        with tempfile.TemporaryDirectory() as tmp:
            master = os.path.join(tmp, "master.img")
            touch(master, b"card")
            with self.assertRaises(ValueError):
                gen_emu.clone_card(master, master)

    def test_smoke_leaves_with_capture_reloads_and_checks_the_saved_song(self):
        with tempfile.TemporaryDirectory() as tmp:
            master = os.path.join(tmp, "master.img")
            touch(master, b"master")
            out = os.path.join(tmp, "out")
            args = gen_emu.parser().parse_args(
                ["smoke", "--no-leak", "--card", master, "--out", out, "--bars", "1"]
            )
            taps = []
            emu = mock.Mock()
            emu.led.side_effect = lambda name: (
                name == "PLAY" and ("PLAY_RELOAD" in taps)
            )
            emu.card_quiet.return_value = True
            emu.dump_size.return_value = 0
            emu.audio.return_value = pcm(2.0)

            def tap(name, **kw):
                if name == "PLAY" and "LOAD" in taps:
                    taps.append("PLAY_RELOAD")
                else:
                    taps.append(name)

            emu.tap.side_effect = tap

            def fake_boot(a, elf, o, tag, *, card=None, snapshot=True):
                self.boot_card = card
                self.boot_snapshot = snapshot
                touch(card, b"cloned")
                return emu

            song_xml = (
                b'<?xml version="1.0" encoding="UTF-8"?>'
                b"<song><instruments>"
                b'<kit presetName="DRUMS" presetFolder="KITS" clipInstances="0x000000000000060000000000" />'
                b"</instruments><sessionClips>"
                b'<instrumentClip instrumentPresetName="DRUMS" length="1536"><noteRows>'
                b'<noteRow drumIndex="0" noteDataWithSplitProb="0x0000000000000006584014000000" />'
                b"</noteRows></instrumentClip></sessionClips></song>"
            )

            class FakeFat:
                def __init__(self, path):
                    self.path = path

                def __enter__(self):
                    return self

                def __exit__(self, *exc):
                    return False

                def exists(self, folder):
                    return folder == gen_emu.SONG_FOLDER

                def listdir(self, folder):
                    return (
                        ["GEN ACID TECHNO 1A2B3C4 LIVE.XML"]
                        if self.path == self.outer.boot_card
                        else []
                    )

                def read_file(self, path):
                    return song_xml

            FakeFat.outer = self

            with (
                mock.patch.object(gen_emu, "warn_pin", return_value={}),
                mock.patch.object(gen_emu, "boot", side_effect=fake_boot),
                mock.patch.object(gen_emu, "enter_perform", return_value=True),
                mock.patch.object(gen_emu, "check_perform_plays"),
                mock.patch.object(gen_emu.fat_image, "FatImage", FakeFat),
                mock.patch.object(gen_emu, "log"),
            ):
                code = gen_emu.smoke(args)

            self.assertEqual(code, 0)
            self.assertFalse(self.boot_snapshot)
            self.assertEqual(self.boot_card, os.path.join(out, "smoke.img"))
            self.assertFalse(os.path.exists(self.boot_card))
            # Leave with LAST (no encoder turn), then LOAD twice to reload the saved song, then PLAY to check it sounds
            emu.s.encoder.assert_not_called()
            self.assertEqual(
                taps,
                [
                    "PLAY",
                    "BACK",
                    "SELECT_ENC",
                    "LOAD",
                    "LOAD",
                    "PLAY_RELOAD",
                    "PLAY_RELOAD",
                ],
            )


if __name__ == "__main__":
    unittest.main()
