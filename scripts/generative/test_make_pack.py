"""Tests for make_pack.py --check, on a copy of the repo's pack/ in a scratch git repo that ignores .DS_Store.

python3 scripts/generative/test_make_pack.py
"""

import contextlib
import hashlib
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

    def test_without_the_factory_card_a_missing_derived_preset_is_stale(self):
        derived = next(iter(make_pack.DERIVED))
        os.remove(os.path.join(self.pack, derived))

        self.assertEqual(
            self.check("--factory", os.path.join(self.tmp, "no card")),
            (1, ["stale: " + derived]),
        )

    def test_a_derived_preset_with_no_licence_row_fails(self):
        derived = next(iter(make_pack.DERIVED))
        path = os.path.join(self.pack, "LICENSES.md")
        with open(path, encoding="utf-8") as f:
            text = f.read()
        kept = [line for line in text.splitlines(True) if f"`{derived}`" not in line]
        with open(path, "w", encoding="utf-8") as f:
            f.write("".join(kept))

        code, lines = self.check("--factory", os.path.join(self.tmp, "no card"))

        self.assertEqual(code, 1)
        self.assertEqual(len(lines), 1, lines)
        self.assertTrue(lines[0].startswith("unlisted: " + derived), lines)


class DerivedTest(unittest.TestCase):
    SOURCE = (
        "<sound>\n\t<defaultParams>\n\t\t<volume>0x74000000</volume>\n"
        "\t\t<delayFeedback>0xE6000000</delayFeedback>\n\t\t<patchCables>\n\t\t\t<patchCable>\n"
        "\t\t\t\t<source>velocity</source>\n\t\t\t\t<destination>volume</destination>\n"
        "\t\t\t\t<amount>0x3FFFFFE8</amount>\n\t\t\t</patchCable>\n\t\t</patchCables>\n"
        "\t</defaultParams>\n</sound>\n"
    )

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        os.makedirs(os.path.join(self.tmp, "SYNTHS"))
        with open(os.path.join(self.tmp, "SYNTHS", "A.XML"), "w") as f:
            f.write(self.SOURCE)
        self.sha = hashlib.sha256(self.SOURCE.encode()).hexdigest()

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def test_applies_its_edits_and_nothing_else(self):
        derived = make_pack.Derived(
            "SYNTHS/A.XML",
            self.sha,
            make_pack.Field("delayFeedback", make_pack.knob(0)),
            make_pack.Cable("velocity", "volume", 25),
        )

        out = derived.make(self.tmp).decode()

        expected = self.SOURCE.replace("0xE6000000", "0x80000000").replace(
            "0x3FFFFFE8", make_pack.cable(25)
        )
        self.assertEqual(out, expected)

    def test_transpose_sets_one_oscillators_transpose_alone(self):
        source = (
            "<sound>\n\t<osc1>\n\t\t<transpose>0</transpose>\n\t</osc1>\n"
            "\t<osc2>\n\t\t<transpose>-17</transpose>\n\t</osc2>\n</sound>\n"
        )

        out = make_pack.Transpose("osc2", -5).apply(source)

        self.assertEqual(out, source.replace("-17", "-5"))

    def test_transpose_refuses_an_element_with_no_transpose(self):
        with self.assertRaisesRegex(ValueError, "0 matches"):
            make_pack.Transpose("osc1", 12).apply(
                "<sound>\n\t<osc1>\n\t</osc1>\n</sound>\n"
            )

    def test_refuses_a_source_that_changed(self):
        derived = make_pack.Derived("SYNTHS/A.XML", "0" * 64)

        with self.assertRaisesRegex(ValueError, "SHA-256"):
            derived.make(self.tmp)

    def test_refuses_an_edit_that_matches_nothing(self):
        derived = make_pack.Derived(
            "SYNTHS/A.XML", self.sha, make_pack.Field("reverbAmount", "0")
        )

        with self.assertRaisesRegex(ValueError, "0 matches"):
            derived.make(self.tmp)


if __name__ == "__main__":
    unittest.main()
