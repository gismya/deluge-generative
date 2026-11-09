"""Tests for decode_pad_crash.py's commit check, on small fake ELF files.

python3 scripts/generative/test_decode_pad_crash.py
"""

import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
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


if __name__ == "__main__":
    unittest.main()
