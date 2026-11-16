"""Tests for decode_pad_crash.py's commit check, on small fake ELF files, and its address lookup.

python3 scripts/generative/test_decode_pad_crash.py
"""

import os
import sys
import tempfile
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import arm_tools
import decode_pad_crash as dpc


def fake_elf(body):
    with tempfile.NamedTemporaryFile(suffix=".elf", delete=False) as fh:
        fh.write(b"\x7fELF" + b"\x00" * 12 + body)
    return fh.name


class BuiltCommits(unittest.TestCase):
    def commits(self, body):
        path = fake_elf(body)
        try:
            return dpc.built_commits(path)
        finally:
            os.unlink(path)

    def test_a_commit_string_of_its_own(self):
        self.assertIn("3212a869c", self.commits(b"\x00MIN\x003212a869c\x00FREE\x00"))

    def test_a_commit_merged_into_the_version_string(self):
        # The linker's string tail-merging keeps one copy: kCommitShort points into the version string's tail.
        commits = self.commits(
            b"\x00 MIN\x00\x00\x00\x001.3.0-dev-3212a869c\x00FREE\x00"
        )
        self.assertIn("3212a869c", commits)

    def test_a_commit_merged_into_a_release_version_string(self):
        self.assertIn("4623ba9f1", self.commits(b"\x001.3.0-4623ba9f1\x00"))

    def test_hex_inside_other_text_is_not_a_commit(self):
        self.assertEqual(
            self.commits(b"\x00deadbeef12x\x00abc-12\x00x0123456789\x00"), []
        )


class Locate(unittest.TestCase):
    NM = (
        "20000000 T program_code_start\n"
        "20001001 00000010 T first()\n"
        "20001010 00000004 d data\n"
        "20001013 00000008 t second()\n"
        "20002000 T program_code_end\n"
    )

    def locate(self, address):
        table = [s._replace(size=s.size or 0) for s in arm_tools.parse_nm(self.NM)]
        return dpc.locate(table, arm_tools.CodeSymbols(table), address)

    def test_names_the_code_symbol_and_the_offset_skipping_data(self):
        where, function, pointer_to = self.locate(0x20001004)
        self.assertEqual(where, "first()+0x4")
        self.assertEqual((function.address, function.size), (0x20001000, 0x10))
        self.assertIsNone(pointer_to)

    def test_two_bytes_before_a_function_past_the_last_ones_end_is_a_pointer(self):
        self.assertEqual(
            self.locate(0x20001010), ("(2 bytes before second())", None, "second()")
        )

    def test_outside_the_code_says_so(self):
        self.assertIn("outside the code", self.locate(0x30000000)[0])


class Tool(unittest.TestCase):
    def test_finds_the_toolchain_as_arm_tools_does_or_exits_saying_where(self):
        with mock.patch.object(
            arm_tools, "find", lambda name: f"/tc/arm-none-eabi-{name}"
        ):
            self.assertEqual(dpc.tool("nm"), "/tc/arm-none-eabi-nm")
        missing = mock.Mock(
            side_effect=FileNotFoundError("no arm-none-eabi-nm on PATH")
        )
        with (
            mock.patch.object(arm_tools, "find", missing),
            self.assertRaises(SystemExit) as caught,
        ):
            dpc.tool("nm")
        self.assertIn("no arm-none-eabi-nm", str(caught.exception))


if __name__ == "__main__":
    unittest.main()
