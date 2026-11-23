"""Tests for test_port_emu.py's script runs, against test_deluge_probe.py's fake Deluge (no emulator).

python3 scripts/generative/test_test_port_emu.py
"""

import contextlib
import io
import os
import sys
import tempfile
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import deluge_probe
import fat_image
import test_port_emu
from test_deluge_probe import FakeDeluge
from test_fat_image import make_fat32


class FakeEmu:
    def __init__(self, link):
        self.port = deluge_probe.TestPort(link, on_hardware=False)  # as EmuRun's

    def alive(self):
        return True


class Events(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.script = os.path.join(self.dir.name, "x.txt")
        with open(self.script, "w") as fh:
            fh.write("start_state\nbutton BACK down\nexpect event gen_left\n")
        self.link = FakeDeluge(
            events={
                "button BACK down": ["gen_left delete_us=300", "gen_mem after_leave"]
            }
        )

    def tearDown(self):
        self.dir.cleanup()

    def run_with(self, events):
        out = []
        test_port_emu.run_scripts(
            FakeEmu(self.link), [self.script], out=out.append, events=events
        )
        return out

    def test_prints_the_scripts_events_that_match_after_it(self):
        # 1.0001 is start_state's gen_recipe_reset, 1.0002 the gen_left
        out = self.run_with("gen_mem")
        self.assertIn("x.txt: event 1.0003: gen_mem after_leave", out)
        self.assertFalse(
            [line for line in out if ": event " in line and "gen_left" in line]
        )
        self.assertTrue(
            out.index("x.txt: event 1.0003: gen_mem after_leave") < len(out) - 1
        )

    def test_a_dot_prints_them_all(self):
        out = self.run_with(".")
        self.assertEqual(len([line for line in out if ": event " in line]), 2)

    def test_without_the_option_none_are_printed(self):
        self.assertFalse([line for line in self.run_with(None) if ": event " in line])


class EmulatorSlot(unittest.TestCase):
    """EmuRun holds a real-time emulator slot (emu_slots.py, #417) from before its first launch until its emulator has
    stopped."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.did = []

    def tearDown(self):
        self.tmp.cleanup()

    def taker(self):
        did = self.did

        class Slot:
            fd = 99

        @contextlib.contextmanager
        def take(out):
            did.append("take")
            try:
                yield Slot()
            finally:
                did.append("release")

        return take

    def run_emu(self, boots):
        emu = test_port_emu.EmuRun(
            "x.elf",
            self.tmp.name,
            sd_master=os.path.join(self.tmp.name, "master.img"),
            take_slot=self.taker(),
        )
        tries = iter(boots)

        def launch(attempt):
            self.did.append("launch")
            emu.port = mock.Mock()
            emu.run_log = "boot.log"

        def boot():
            if not next(tries):
                raise RuntimeError("blank")
            return {}

        with (
            mock.patch.object(emu, "_launch", side_effect=launch),
            mock.patch.object(emu, "_wait_for_boot", side_effect=boot),
            mock.patch.object(emu, "_warm_up"),
            mock.patch.object(
                emu, "_stop", side_effect=lambda: self.did.append("stop")
            ),
            mock.patch.object(emu, "save_log"),
            contextlib.redirect_stdout(io.StringIO()),
            emu,
        ):
            self.did.append("run")
        return emu

    def test_takes_a_slot_before_the_launch_and_releases_it_after_the_stop(self):
        self.run_emu([True])
        self.assertEqual(self.did, ["take", "launch", "run", "stop", "release"])

    def test_keeps_one_slot_across_boot_retries(self):
        self.run_emu([False, True])
        self.assertEqual(
            self.did, ["take", "launch", "stop", "launch", "run", "stop", "release"]
        )

    def test_a_run_with_no_boot_releases_its_slot(self):
        with self.assertRaises(RuntimeError):
            self.run_emu([False] * test_port_emu.BOOT_TRIES)
        self.assertEqual(self.did[0], "take")
        self.assertEqual(self.did[-1], "release")
        self.assertEqual(self.did.count("release"), 1)

    def test_the_emulator_inherits_the_slots_lock(self):
        emu = test_port_emu.EmuRun(
            "x.elf",
            self.tmp.name,
            sd_master=os.path.join(self.tmp.name, "master.img"),
            take_slot=self.taker(),
        )
        emu.slot = mock.Mock(fd=42)
        with (
            mock.patch.object(test_port_emu.subprocess, "run"),
            mock.patch.object(test_port_emu.subprocess, "Popen") as popen,
            mock.patch.object(test_port_emu.deluge_probe, "UdpLink"),
            mock.patch.object(test_port_emu, "free_udp_port", return_value=5000),
        ):
            emu._launch(1)
            emu.log_file.close()
        self.assertEqual(popen.call_args.kwargs["pass_fds"], [42])


class Stopped(unittest.TestCase):
    def test_a_script_that_stops_on_an_error_says_how_far_it_got(self):
        with tempfile.TemporaryDirectory() as tmp:
            script = os.path.join(tmp, "x.txt")
            with open(script, "w") as fh:
                fh.write("start_state\nexpect playing=false\nno such\n")
            out = []
            results = test_port_emu.run_scripts(
                FakeEmu(FakeDeluge()), [script], out=out.append
            )
        self.assertEqual(results[0][1:3], (1, 3))
        self.assertIn(
            "stopped after 2 passed and 0 failed: line 3: unknown command",
            results[0][3],
        )

    def test_a_stopped_script_prints_its_last_events_as_the_session_does(self):
        with tempfile.TemporaryDirectory() as tmp:
            script = os.path.join(tmp, "x.txt")
            with open(script, "w") as fh:
                fh.write("button BACK down\nno such\n")
            out = []
            link = FakeDeluge(events={"button BACK down": ["gen_left delete_us=300"]})
            test_port_emu.run_scripts(FakeEmu(link), [script], out=out.append)
        self.assertIn("x.txt: ERROR line 2: unknown command 'no'", out)
        self.assertIn("x.txt: the last 1 events before it stopped, of any kind:", out)
        self.assertIn("x.txt: last 1.0001: gen_left delete_us=300", out)


class HardwareOnly(unittest.TestCase):
    SCRIPT = (
        "start_state\n"
        "hardware_only expect no_event gen_h16 culls=0 within=0.1\n"
        "hardware_only expect bpm=120\n"
    )

    def test_each_scripts_skips_are_counted_in_its_result(self):
        with tempfile.TemporaryDirectory() as tmp:
            script = os.path.join(tmp, "x.txt")
            with open(script, "w") as fh:
                fh.write(self.SCRIPT)
            out = []
            results = test_port_emu.run_scripts(
                FakeEmu(FakeDeluge()), [script, script], out=out.append
            )
        self.assertEqual(results[1][1:], (0, 1, None, 2))
        self.assertIn("x.txt: 1 expects, 0 failed, 2 hardware-only skipped", out)

    def test_the_summary_line_counts_them_for_check_all(self):
        class Booted:
            def __init__(self, *args, **kw):
                pass

            def __enter__(self):
                return FakeEmu(FakeDeluge())

            def __exit__(self, *exc):
                pass

        with tempfile.TemporaryDirectory() as tmp:
            script = os.path.join(tmp, "x.txt")
            with open(script, "w") as fh:
                fh.write(self.SCRIPT)
            elf = os.path.join(tmp, "deluge.elf")
            open(elf, "w").close()
            argv = ["test_port_emu.py", "--out", tmp, "--elf", elf, "--script", script]
            argv.append("--no-protocol-checks")
            printed = io.StringIO()
            with (
                mock.patch.object(test_port_emu, "EmuRun", Booted),
                mock.patch.object(sys, "argv", argv),
                contextlib.redirect_stdout(printed),
            ):
                self.assertEqual(test_port_emu.main(), 0)
        self.assertIn(
            f"1 checks, 0 failed, 2 hardware-only skipped; logs in {tmp}",
            printed.getvalue(),
        )
        self.assertIn(
            "  x.txt: 1 expects, 0 failed, 2 hardware-only skipped", printed.getvalue()
        )


class ScriptsCard(unittest.TestCase):
    """With no --card-overlay, the card comes from the scripts' "# needs: card DIR" lines (#294)."""

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = tmp.name

    def script(self, name, needs=""):
        path = os.path.join(self.tmp, name)
        with open(path, "w") as fh:
            fh.write(needs + "start_state\n")
        return path

    def test_scripts_with_no_card_line_run_on_the_factory_card(self):
        self.assertIsNone(test_port_emu.scripts_card([self.script("a.txt")]))

    def test_a_card_line_names_the_overlay(self):
        script = self.script("a.txt", "# needs: card fixtures/a (its own card)\n")
        self.assertEqual(
            test_port_emu.scripts_card([script, script]),
            os.path.join(self.tmp, "fixtures", "a"),
        )

    def test_scripts_on_different_cards_are_refused(self):
        a = self.script("a.txt", "# needs: card fixtures/a\n")
        b = self.script("b.txt")
        with self.assertRaisesRegex(ValueError, "separately: a.txt .*, b.txt "):
            test_port_emu.scripts_card([a, b])


class Main(unittest.TestCase):
    def test_an_os_error_from_the_card_is_an_error_line_not_a_traceback(self):
        class FullCard:
            def __init__(self, *args, **kw):
                pass

            def __enter__(self):
                raise OSError("the card image is full")

            def __exit__(self, *exc):
                pass

        with tempfile.TemporaryDirectory() as tmp:
            elf = os.path.join(tmp, "deluge.elf")
            open(elf, "w").close()
            argv = ["test_port_emu.py", "--out", tmp, "--elf", elf]
            printed = io.StringIO()
            with (
                mock.patch.object(test_port_emu, "EmuRun", FullCard),
                mock.patch.object(sys, "argv", argv),
                contextlib.redirect_stdout(printed),
            ):
                self.assertEqual(test_port_emu.main(), 1)
        self.assertIn("ERROR the card image is full", printed.getvalue())


class CardOverlay(unittest.TestCase):
    """--card-overlay DIR: a card image built from the factory card and a folder kept in the repo (#277)."""

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = tmp.name
        self.card = os.path.join(self.tmp, "card.img")
        make_fat32(self.card)
        with fat_image.FatImage(self.card) as card:
            card.write_file("SYNTHS/165 Acid Arp.XML", b"acid")
            card.write_file("SYNTHS/047 Basic Dirty Bass.XML", b"dirty")
            card.write_file("SAMPLES/DRUMS/Kick/808 Kick.wav", b"RIFFkick")
        self.repo = os.path.join(self.tmp, "repo")
        self.write(
            self.repo,
            "fixtures/palettes/GENERATIVE/PALETTES/ACID TECHNO.XML",
            b"<acid/>",
        )
        self.overlay = os.path.join(self.repo, "overlay")

    @staticmethod
    def write(root, path, data):
        full = os.path.join(root, *path.split("/"))
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "wb") as fh:
            fh.write(data)

    def apply(self, steps):
        self.write(self.overlay, "card.txt", steps.encode())
        return test_port_emu.apply_overlay(self.card, self.overlay, repo=self.repo)

    def test_runs_its_steps_then_copies_its_own_files(self):
        self.write(self.overlay, "SAMPLES/DRUMS/Rim/My Rim.wav", b"RIFFrim")
        self.apply(
            "# the fixtures, a kick of the player's own, no acid preset\n"
            "remove SYNTHS/165 Acid Arp.XML\n"
            "copy fixtures/palettes -> /\n"
            "copy card:SAMPLES/DRUMS/Kick/808 Kick.wav -> SAMPLES/DRUMS/MyKicks/Mine 1.wav\n"
        )
        with fat_image.FatImage(self.card) as card:
            self.assertEqual(card.listdir("SYNTHS"), ["047 Basic Dirty Bass.XML"])
            self.assertEqual(
                card.read_file("GENERATIVE/PALETTES/ACID TECHNO.XML"), b"<acid/>"
            )
            self.assertEqual(
                card.read_file("SAMPLES/DRUMS/MyKicks/Mine 1.wav"), b"RIFFkick"
            )
            self.assertEqual(card.read_file("SAMPLES/DRUMS/Rim/My Rim.wav"), b"RIFFrim")
            self.assertFalse(card.exists("card.txt"))

    def test_a_copy_to_a_folder_goes_under_it(self):
        self.apply("copy fixtures/palettes/GENERATIVE -> GENERATIVE\n")
        with fat_image.FatImage(self.card) as card:
            self.assertEqual(card.listdir("GENERATIVE/PALETTES"), ["ACID TECHNO.XML"])

    def test_removing_what_the_card_doesnt_have_is_an_error(self):
        # The factory card changed under the overlay: say so rather than test another card.
        with self.assertRaisesRegex(FileNotFoundError, "SYNTHS/999 Gone.XML"):
            self.apply("remove SYNTHS/999 Gone.XML\n")

    def test_a_bad_step_is_an_error(self):
        with self.assertRaisesRegex(ValueError, "card.txt line 1: "):
            self.apply("copy fixtures/palettes\n")

    def test_a_copy_from_outside_the_repo_is_an_error(self):
        self.write(self.tmp, "secret.txt", b"x")
        with self.assertRaisesRegex(
            ValueError, "card.txt line 1: ../secret.txt is outside the repo"
        ):
            self.apply("copy ../secret.txt -> SECRET.TXT\n")

    def test_a_missing_overlay_is_an_error(self):
        with self.assertRaises(FileNotFoundError):
            test_port_emu.apply_overlay(self.card, os.path.join(self.tmp, "nope"))


if __name__ == "__main__":
    unittest.main()
