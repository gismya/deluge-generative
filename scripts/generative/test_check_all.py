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


if __name__ == "__main__":
    unittest.main()
