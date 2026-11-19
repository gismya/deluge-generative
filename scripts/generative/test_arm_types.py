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
# The same mix in a header no .cpp includes: it fails only when the header is compiled on its own.
CLAMP_HEADER = "#pragma once\n" + CLAMP.replace("int32_t f(", "inline int32_t f(")
# A switch that misses an enumerator: a warning on the host, an error in the firmware (-Werror=switch).
SWITCH = """enum class Kind { a, b };
int f(Kind k) {
  switch (k) {
  case Kind::a:
    return 1;
  }
  return 0;
}
"""


class Sources(unittest.TestCase):
    def test_every_portable_cpp_and_no_firmware_adapter(self):
        found = arm_types.sources()
        self.assertIn("src/deluge/generative/core/generator.cpp", found)
        self.assertTrue(
            any(p.startswith("src/deluge/generative/perform/") for p in found)
        )
        self.assertTrue(
            any(p.startswith("src/deluge/generative/compose/") for p in found)
        )
        self.assertTrue(
            any(p.startswith("src/deluge/generative/runner/") for p in found)
        )
        self.assertFalse(any("/generative/firmware/" in p for p in found), found)
        self.assertTrue(all(p.endswith((".cpp", ".h")) for p in found))

    def test_headers_too_even_those_no_portable_cpp_includes(self):
        found = arm_types.sources()
        self.assertIn("src/deluge/generative/perform/perform_steering.h", found)
        self.assertIn("src/deluge/generative/core/generator.h", found)

    def test_a_header_is_compiled_as_cpp_and_a_cpp_as_itself(self):
        self.assertEqual(arm_types.command("g++", "a.h")[-3:], ["-x", "c++", "a.h"])
        self.assertNotIn("-x", arm_types.command("g++", "a.cpp"))
        self.assertIn("-Werror=switch", arm_types.command("g++", "a.cpp"))


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

    def test_a_type_error_in_a_header_fails(self):
        bad = self.write("bad.h", CLAMP_HEADER)
        failed = arm_types.check([bad], self.gxx)
        self.assertEqual(list(failed), [bad])
        self.assertIn("clamp", failed[bad])

    def test_a_switch_missing_an_enumerator_fails_as_in_the_firmware(self):
        bad = self.write("switch.cpp", SWITCH)
        failed = arm_types.check([bad], self.gxx)
        self.assertEqual(list(failed), [bad])
        self.assertIn("-Werror=switch", failed[bad])

    def test_main_exits_1_and_shows_the_error(self):
        bad = self.write("bad.cpp", CLAMP)
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            self.assertEqual(arm_types.main([bad]), 1)
        self.assertIn("1 files on ARM, 1 with errors", out.getvalue())
        self.assertIn("clamp", err.getvalue())

    def test_a_relative_path_counts_from_where_it_was_given_not_the_repo(self):
        self.write("fine.cpp", FINE)
        out = io.StringIO()
        old = os.getcwd()
        os.chdir(self.tmp.name)
        try:
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
                self.assertEqual(arm_types.main(["fine.cpp"]), 0)
        finally:
            os.chdir(old)
        self.assertIn("1 files on ARM, 0 with errors", out.getvalue())


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
        self.assertIn("run any ./dbt command once", err.getvalue())


if __name__ == "__main__":
    unittest.main()
