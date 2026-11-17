"""Tests for test_port_emu.py's script runs, against test_deluge_probe.py's fake Deluge (no emulator).

python3 scripts/generative/test_test_port_emu.py
"""

import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import deluge_probe
import fat_image
import test_port_emu
from test_deluge_probe import FakeDeluge
from test_fat_image import make_fat32


class FakeEmu:
    def __init__(self, link):
        self.port = deluge_probe.TestPort(link)

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

    def test_a_missing_overlay_is_an_error(self):
        with self.assertRaises(FileNotFoundError):
            test_port_emu.apply_overlay(self.card, os.path.join(self.tmp, "nope"))


if __name__ == "__main__":
    unittest.main()
