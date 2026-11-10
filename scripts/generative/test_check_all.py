"""Tests for check_all.py's table notes.

python3 scripts/generative/test_check_all.py
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_all

CYCLES = (
    "[gen-emu]   cycle 1: internal 1000 -> 680 (-320 B), external 5000 -> 5000 (+0 B) [warm-up, not checked]\n"
    "[gen-emu]   cycle 2: internal 680 -> 680 (+0 B), external 5000 -> 5000 (+0 B)\n"
)


class LeakDetail(unittest.TestCase):
    def test_a_pass_lists_each_cycles_deltas(self):
        text = CYCLES + "[gen-emu] leak check: PASS (logs in x)\n"
        self.assertEqual(
            check_all.leak_detail(text),
            "internal/external B per cycle: c1 -320/+0, c2 +0/+0",
        )

    def test_counts_every_problem_the_check_counted_not_only_cycle_ones(self):
        text = (
            CYCLES
            + "[gen-emu] FAIL only 2 of 4 enter/leave cycles completed (see the script output)\n"
            + "[gen-emu] FAIL 3 expects failed in the cycles\n"
            + "[gen-emu] leak check: FAIL, 2 problems (logs in x)\n"
        )
        self.assertEqual(
            check_all.leak_detail(text),
            "2 problems; internal/external B per cycle: c1 -320/+0, c2 +0/+0",
        )

    def test_a_check_that_never_got_to_the_cycles_says_why(self):
        text = "[gen-emu] FAIL leak check: no reply to test port command 0x11\n"
        self.assertEqual(
            check_all.leak_detail(text), "no reply to test port command 0x11"
        )


class FailedExpects(unittest.TestCase):
    def test_picks_each_fail_line_with_the_got_lines_under_it(self):
        lines = [
            "[test-port-emu] x.txt: PASS line 3: expect playing=true",
            "[test-port-emu] x.txt: FAIL line 4: expect event gen_edit control=none",
            "[test-port-emu] x.txt:   got no gen_edit event",
            "[test-port-emu] x.txt: PASS line 5: expect voices=0",
            "[test-port-emu] x.txt: FAIL line 9: expect no_event gen_midi_feedback within=3",
            "[test-port-emu] x.txt:   got 2.5: gen_midi_feedback ccs=1",
            "[test-port-emu]   x.txt: 9 expects, 2 failed",
        ]
        self.assertEqual(
            check_all.failed_expects(lines), [lines[1], lines[2], lines[4], lines[5]]
        )

    def test_a_passing_log_picks_nothing(self):
        self.assertEqual(
            check_all.failed_expects(
                ["[test-port-emu] x.txt: PASS line 3: expect playing=true"]
            ),
            [],
        )


class ResolveScriptTest(unittest.TestCase):
    def test_a_bare_name_is_found_in_scripts_generative(self):
        self.assertEqual(
            check_all.resolve_script("perform_homes.txt"),
            os.path.join(check_all.HERE, "perform_homes.txt"),
        )

    def test_a_path_is_taken_as_given(self):
        path = os.path.join(check_all.HERE, "perform_homes.txt")
        self.assertEqual(check_all.resolve_script(os.path.relpath(path)), path)

    def test_a_missing_script_stops_the_run_naming_it(self):
        with self.assertRaises(SystemExit) as stopped:
            check_all.resolve_script("perform_nothing.txt")
        self.assertIn("perform_nothing.txt", str(stopped.exception.code))


if __name__ == "__main__":
    unittest.main()
