"""Tests for make_pack.py --check, on a copy of the repo's pack/.

python3 scripts/generative/test_make_pack.py
"""

import contextlib
import io
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import make_pack


class MakePackCheckTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.pack = os.path.join(self.tmp, "pack")
        shutil.copytree(make_pack.PACK, self.pack)

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def check(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = make_pack.main(["--check", "--pack", self.pack])
        return code, out.getvalue().splitlines()

    def write(self, rel, data=b"x"):
        path = os.path.join(self.pack, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)

    def test_the_committed_pack_passes(self):
        self.assertEqual(self.check(), (0, []))

    def test_a_file_the_script_does_not_write_fails(self):
        self.write("SAMPLES/DRUMS/Clap/Borrowed clap.wav")

        code, lines = self.check()

        self.assertEqual(code, 1)
        self.assertEqual(len(lines), 1, lines)
        self.assertTrue(
            lines[0].startswith("unexpected: SAMPLES/DRUMS/Clap/Borrowed clap.wav"),
            lines,
        )

    def test_licences_and_finder_files_are_expected(self):
        self.write(".DS_Store")
        self.write("SYNTHS/.DS_Store")

        self.assertEqual(self.check(), (0, []))
        self.assertTrue(os.path.isfile(os.path.join(self.pack, "LICENSES.md")))

    def test_a_changed_file_is_stale(self):
        self.write("SYNTHS/GENERATIVE/FX/GEN Riser.XML", b"<sound/>")

        self.assertEqual(
            self.check(), (1, ["stale: SYNTHS/GENERATIVE/FX/GEN Riser.XML"])
        )


if __name__ == "__main__":
    unittest.main()
