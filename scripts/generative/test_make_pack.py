"""Tests for make_pack.py --check, on a copy of the repo's pack/ in a scratch git repo that ignores .DS_Store.

python3 scripts/generative/test_make_pack.py
"""

import contextlib
import io
import os
import shutil
import subprocess
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
        subprocess.run(["git", "init", "-q", self.tmp], check=True)
        with open(os.path.join(self.tmp, ".gitignore"), "w") as f:
            f.write(".DS_Store\n")

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def check(self, *flags):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = make_pack.main(["--check", "--pack", self.pack, *flags])
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

    def test_licences_and_finder_files_are_expected_in_the_work_tree(self):
        self.write(".DS_Store")
        self.write("SYNTHS/.DS_Store")
        self.write("SAMPLES/FX/Riser/._GEN Riser 1.wav")

        self.assertEqual(self.check(), (0, []))
        self.assertTrue(os.path.isfile(os.path.join(self.pack, "LICENSES.md")))

    def test_a_gitignored_file_is_passed_over_in_the_work_tree(self):
        with open(os.path.join(self.tmp, ".gitignore"), "a") as f:
            f.write("*.bak\n")
        self.write("SYNTHS/GENERATIVE/FX/GEN Riser.XML.bak")

        self.assertEqual(self.check(), (0, []))

    def test_a_committed_tree_counts_every_file(self):
        self.write(".DS_Store")
        self.write("SAMPLES/FX/Riser/._GEN Riser 1.wav")

        code, lines = self.check("--committed")

        self.assertEqual(code, 1)
        self.assertEqual(
            [line.split(" (")[0] for line in lines],
            ["unexpected: .DS_Store", "unexpected: SAMPLES/FX/Riser/._GEN Riser 1.wav"],
        )

    def test_a_changed_file_is_stale(self):
        self.write("SYNTHS/GENERATIVE/FX/GEN Riser.XML", b"<sound/>")

        self.assertEqual(
            self.check(), (1, ["stale: SYNTHS/GENERATIVE/FX/GEN Riser.XML"])
        )


if __name__ == "__main__":
    unittest.main()
