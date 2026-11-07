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
                "techno",
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
            '{.genre = "techno", .entropy = 0u, .seed = 7u, .macroPoints = {40, -1, -1, -1}, '
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

    def test_a_capture_is_named_after_every_argument_and_never_overwrites(self):
        args = gen_emu.parser().parse_args(
            ["capture", "--seed", "7", "--entropy", "2", "--energy", "40", "--motion", "0", "--key", "C#",
             "--scale", "phrygian", "--tempo", "131.5", "--swing", "-10", "--seconds", "30"]
        )  # fmt: skip
        with tempfile.TemporaryDirectory() as tmp:
            first = gen_emu.capture_path(args, tmp)
            self.assertEqual(
                os.path.basename(first),
                "techno-seed7-entropy2-energy40-motion0-keyC#-phrygian-tempo131.5-swing-10-30s.wav",
            )
            open(first, "w").close()
            second = gen_emu.capture_path(args, tmp)
            self.assertEqual(
                os.path.basename(second), os.path.basename(first)[:-4] + "-2.wav"
            )
            plain = gen_emu.capture_path(gen_emu.parser().parse_args(["capture"]), tmp)
            self.assertEqual(os.path.basename(plain), "techno-seed1-60s.wav")

    def test_it_knows_the_genres_and_scales_the_firmware_has(self):
        self.assertIn("techno", gen_emu.genre_names())
        scales = gen_emu.scale_names()
        self.assertEqual(scales[:4], ["major", "minor", "dorian", "phrygian"])
        self.assertIn("harmonic-minor", scales)
        self.assertEqual(len(scales), 12)

    def test_unknown_genre_scale_or_key_is_refused(self):
        for argv in (["--genre", "polka"], ["--scale", "blues"], ["--key", "H"]):
            with self.assertRaises(SystemExit):
                gen_emu.capture_header(gen_emu.parser().parse_args(["capture", *argv]))


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
                mem("after_leave", 900, 400 - 104),
            ]
        cycles = gen_emu.mem_cycles(events)
        lines, failures = gen_emu.leak_report(cycles, wanted=4)
        self.assertEqual(failures, [])
        self.assertIn("warm-up", lines[0])

    def test_any_internal_loss_or_external_past_the_stock_loss_fails(self):
        events = [mem("before_entry", 1000, 500), mem("after_leave", 1000, 500)]
        events += [mem("before_entry", 1000, 500), mem("after_leave", 992, 500)]
        events += [mem("before_entry", 992, 500), mem("after_leave", 992, 500 - 105)]
        _, failures = gen_emu.leak_report(gen_emu.mem_cycles(events), wanted=3)
        self.assertEqual(len(failures), 2)
        self.assertIn("cycle 2: internal free memory changed by -8 B", failures[0])
        self.assertIn("cycle 3: external lost 105 B", failures[1])

    def test_missing_cycles_fail(self):
        events = [mem("before_entry", 1000, 500), mem("after_leave", 1000, 500)]
        _, failures = gen_emu.leak_report(gen_emu.mem_cycles(events), wanted=3)
        self.assertTrue(any("only 1 of 3" in f for f in failures))
        self.assertTrue(any("no cycle after" in f for f in failures))


if __name__ == "__main__":
    unittest.main()
