"""Tests for make_pack.py --check, on a copy of the repo's pack/ in a scratch git repo that ignores .DS_Store.

python3 scripts/generative/test_make_pack.py
"""

import contextlib
import hashlib
import io
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import wave

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


class KnobTest(unittest.TestCase):
    # As the sound editor saves a menu value (computeFinalValueForStandardMenuItem): the top is INT32_MAX exactly, so
    # a check against full (sound_controls.cpp's kMenuFull) sees a full knob as full (#483).
    def test_the_top_is_full_scale(self):
        self.assertEqual(make_pack.knob(50), "0x7FFFFFFF")

    def test_the_bottom_and_the_middle_follow_the_menu(self):
        self.assertEqual(make_pack.knob(0), "0x80000000")
        self.assertEqual(make_pack.knob(25), "0xFFFFFFE9")
        self.assertEqual(make_pack.knob(49), "0x7AE14781")


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


class LeadTrimTest(unittest.TestCase):
    """A derived sample: a factory WAV with its leading silence cut (Dub sign-off 1's snare)."""

    FRAMES = ((0, 0), (1, -1), (2, -2), (1000, -1000), (2000, -2000), (3000, -3000))

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        os.makedirs(os.path.join(self.tmp, "SAMPLES"))
        self.path = os.path.join(self.tmp, "SAMPLES", "A.wav")
        with wave.open(self.path, "wb") as w:
            w.setnchannels(2)
            w.setsampwidth(2)
            w.setframerate(44100)
            w.writeframes(b"".join(struct.pack("<hh", *f) for f in self.FRAMES))
        with open(self.path, "rb") as f:
            self.sha = hashlib.sha256(f.read()).hexdigest()

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def frames(self, data):
        with wave.open(io.BytesIO(data)) as w:
            params = (w.getnchannels(), w.getsampwidth(), w.getframerate())
            raw = w.readframes(w.getnframes())
        return params, [
            struct.unpack_from("<hh", raw, i) for i in range(0, len(raw), 4)
        ]

    def test_cuts_the_lead_and_fades_the_rest_in_keeping_the_format(self):
        derived = make_pack.Derived("SAMPLES/A.wav", self.sha, make_pack.LeadTrim(3, 2))

        params, frames = self.frames(derived.make(self.tmp))

        self.assertEqual(params, (2, 2, 44100))
        self.assertEqual(frames, [(0, 0), (1000, -1000), (3000, -3000)])

    def test_refuses_a_cut_past_the_end(self):
        derived = make_pack.Derived("SAMPLES/A.wav", self.sha, make_pack.LeadTrim(6, 0))

        with self.assertRaisesRegex(ValueError, "frames"):
            derived.make(self.tmp)

    def test_the_pack_snare_starts_on_its_attack(self):
        derived = make_pack.DERIVED["SAMPLES/DRUMS/Snare/Dub/GEN DDD1 Snare wet.wav"]
        self.assertEqual(derived.source, "SAMPLES/DRUMS/Snare/DDD1 Snare wet.wav")
        trim = derived.edits[0]
        # The factory file's attack (its first frame over -26 dBFS) is at frame 1464, 33.2 ms: the copy keeps under
        # 1 ms before it, as the other Dub snares' files do.
        self.assertTrue(0 < 1464 - trim.frames < 44, trim.frames)


class DelayFeedbackTest(unittest.TestCase):
    """No pack preset saves delay feedback of 25 or more: each repeat's gain is the menu value / 25, and at 25 and over
    the Deluge's delay never abandons in Perform (spec §11.4), so an echo plays on for good (GEN Dub Impact Crack at
    26, the Dub sign-off)."""

    def test_reads_every_delay_feedback_in_menu_units(self):
        xml = (
            f"<sound><defaultParams><delayFeedback>{make_pack.knob(20)}</delayFeedback></defaultParams>"
            f"<kit><delayFeedback>{make_pack.knob(26)}</delayFeedback></kit></sound>"
        )
        self.assertEqual(make_pack.delay_feedbacks(xml), [20, 26])

    def test_reads_a_kits_delay_attribute(self):
        xml = f'<kit><delay pingPong="1" feedback="{make_pack.knob(26)}" rate="0x00000000"/></kit>'
        self.assertEqual(make_pack.delay_feedbacks(xml), [26])
        self.assertTrue(make_pack.runaway_delay(xml))

    def test_flags_a_feedback_of_25_or_more(self):
        for value, runaway in (
            (0, False),
            (24, False),
            (25, True),
            (26, True),
            (50, True),
        ):
            xml = (
                f"<sound><delayFeedback>{make_pack.knob(value)}</delayFeedback></sound>"
            )
            self.assertEqual(make_pack.runaway_delay(xml), runaway, value)

    def test_no_generated_preset_saves_a_runaway_delay(self):
        for rel, make in make_pack.PRESETS.items():
            if rel.endswith(".XML"):
                data = make()
                self.assertFalse(
                    make_pack.runaway_delay(
                        data if isinstance(data, str) else data.decode()
                    ),
                    rel,
                )

    def test_no_preset_in_the_pack_saves_a_runaway_delay(self):
        for root, _, files in os.walk(make_pack.PACK):
            for name in files:
                if name.upper().endswith(".XML") and not name.startswith("._"):
                    with open(
                        os.path.join(root, name), encoding="utf-8", errors="replace"
                    ) as f:
                        self.assertFalse(
                            make_pack.runaway_delay(f.read()), os.path.join(root, name)
                        )


if __name__ == "__main__":
    unittest.main()
