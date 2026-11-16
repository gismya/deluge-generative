"""Tests for arm_types.py: which files it checks, and that an int32_t/int mix fails on ARM (when the firmware's
toolchain is here; CI has none, so that test skips there).

    python3 scripts/generative/test_arm_types.py
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
import arm_tools
import arm_types

# Compiles on the host, where int32_t is int; on ARM int32_t is long and std::clamp finds no match.
CLAMP = """#include <algorithm>
#include <cstdint>
int32_t f(int32_t x) { return std::clamp(x, 0, 5); }
"""
FINE = """#include <algorithm>
#include <cstdint>
int32_t f(int32_t x) { return std::clamp<int32_t>(x, 0, 5); }
"""


class Sources(unittest.TestCase):
    def test_every_portable_cpp_and_no_firmware_adapter(self):
        found = arm_types.sources()
        self.assertIn("src/deluge/generative/core/generator.cpp", found)
        self.assertTrue(
            any(p.startswith("src/deluge/generative/perform/") for p in found)
        )
        self.assertTrue(
            any(p.startswith("src/deluge/generative/runner/") for p in found)
        )
        self.assertFalse(any("/generative/firmware/" in p for p in found), found)
        self.assertTrue(all(p.endswith(".cpp") for p in found))


class OnArm(unittest.TestCase):
    def setUp(self):
        try:
            self.gxx = arm_tools.find("g++")
        except FileNotFoundError:
            self.skipTest("no arm-none-eabi-g++ here")
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def write(self, name, text):
        path = os.path.join(self.tmp.name, name)
        with open(path, "w") as f:
            f.write(text)
        return path

    def test_an_int32_t_and_int_mix_fails_and_a_matched_one_passes(self):
        bad, good = self.write("bad.cpp", CLAMP), self.write("good.cpp", FINE)
        failed = arm_types.check([bad, good], self.gxx)
        self.assertEqual(list(failed), [bad])
        self.assertIn("clamp", failed[bad])

    def test_main_exits_1_and_shows_the_error(self):
        bad = self.write("bad.cpp", CLAMP)
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            self.assertEqual(arm_types.main([bad]), 1)
        self.assertIn("1 files on ARM, 1 with errors", out.getvalue())
        self.assertIn("clamp", err.getvalue())


class NoToolchain(unittest.TestCase):
    def test_says_where_it_looked(self):
        missing = mock.Mock(
            side_effect=FileNotFoundError("no arm-none-eabi-g++ on PATH")
        )
        err = io.StringIO()
        with (
            mock.patch.object(arm_tools, "find", missing),
            contextlib.redirect_stderr(err),
        ):
            self.assertEqual(arm_types.main(["x.cpp"]), 1)
        self.assertIn("no arm-none-eabi-g++", err.getvalue())


if __name__ == "__main__":
    unittest.main()
