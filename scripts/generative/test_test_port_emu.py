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
import test_port_emu
from test_deluge_probe import FakeDeluge


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
        out = self.run_with("gen_mem")
        self.assertIn("x.txt: event 1.0002: gen_mem after_leave", out)
        self.assertFalse([line for line in out if "event 1.0001" in line])
        self.assertTrue(
            out.index("x.txt: event 1.0002: gen_mem after_leave") < len(out) - 1
        )

    def test_a_dot_prints_them_all(self):
        out = self.run_with(".")
        self.assertEqual(len([line for line in out if ": event " in line]), 2)

    def test_without_the_option_none_are_printed(self):
        self.assertFalse([line for line in self.run_with(None) if ": event " in line])


if __name__ == "__main__":
    unittest.main()
