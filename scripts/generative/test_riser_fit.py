"""Tests for riser_fit.py: every pack Riser a genre's Riser lane can pick fits its shortest Build, or its fallback does
(spec §9.5, §10.5).

python3 scripts/generative/test_riser_fit.py
"""

import os
import struct
import sys
import tempfile
import unittest
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import riser_fit

RISERS = "SAMPLES/FX/Riser"


def write_wav(path, seconds, rate=1000):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with wave.open(path, "wb") as f:
        f.setnchannels(1)
        f.setsampwidth(2)
        f.setframerate(rate)
        f.writeframes(struct.pack("<h", 0) * int(seconds * rate))


class RiserFit(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.pack = self.dir.name
        write_wav(os.path.join(self.pack, RISERS, "GEN Riser 1.wav"), 8.0)
        write_wav(os.path.join(self.pack, RISERS, "GEN Riser Short.wav"), 2.5)
        with open(os.path.join(self.pack, RISERS, "notes.txt"), "w") as f:
            f.write("not audio")

    def tearDown(self):
        self.dir.cleanup()

    def test_passes_every_riser_shorter_than_the_build(self):
        ok, lines, count = riser_fit.check(
            f"techno-acid\t14222\t{RISERS}\t\n", self.pack
        )
        self.assertTrue(ok, lines)
        self.assertEqual(count, 2)

    def test_reads_only_the_files_the_lane_tags_reach(self):
        ok, lines, count = riser_fit.check(
            f"dub\t3000\t{RISERS}\tgen riser short\n", self.pack
        )
        self.assertTrue(ok, lines)
        self.assertEqual(count, 1)

    def test_fails_a_riser_longer_than_the_shortest_build(self):
        ok, lines, _ = riser_fit.check(f"dub\t3000\t{RISERS}\t\n", self.pack)
        self.assertFalse(ok)
        self.assertEqual(len(lines), 1)
        self.assertTrue(
            lines[0].startswith("LONG dub: SAMPLES/FX/Riser/GEN Riser 1.wav is 8.00 s"),
            lines,
        )

    def test_passes_a_riser_longer_than_the_shortest_build_whose_fallback_fits(self):
        write_wav(os.path.join(self.pack, RISERS, "GEN Riser 2.wav"), 6.0)
        listing = f"fallback\t{RISERS}/GEN Riser 2.wav\t6000\ntechno-acid\t7111\t{RISERS}\tGEN Riser 1\n"
        ok, lines, count = riser_fit.check(listing, self.pack)
        self.assertTrue(ok, lines)
        self.assertEqual(count, 1)

    def test_fails_a_longer_riser_whose_fallback_doesnt_fit_either(self):
        write_wav(os.path.join(self.pack, RISERS, "GEN Riser 2.wav"), 6.0)
        listing = f"fallback\t{RISERS}/GEN Riser 2.wav\t6000\ntechno-acid\t5000\t{RISERS}\tGEN Riser 1\n"
        ok, lines, _ = riser_fit.check(listing, self.pack)
        self.assertFalse(ok)
        self.assertTrue(lines[0].endswith("and no fallback fits"), lines)

    def test_fails_a_fallback_missing_from_the_pack_or_listed_at_another_length(self):
        ok, lines, _ = riser_fit.check(
            f"fallback\t{RISERS}/GEN Riser 9.wav\t6000\n", self.pack
        )
        self.assertFalse(ok)
        self.assertTrue(lines[0].startswith("FALLBACK"), lines)
        ok, lines, _ = riser_fit.check(
            f"fallback\t{RISERS}/GEN Riser Short.wav\t2000\n", self.pack
        )
        self.assertFalse(ok)  # it is 2.5 s
        self.assertTrue(lines[0].startswith("FALLBACK"), lines)

    def test_fails_a_lane_that_reaches_no_pack_riser(self):
        ok, lines, count = riser_fit.check(f"dub\t3000\t{RISERS}\tSiren\n", self.pack)
        self.assertFalse(ok)
        self.assertEqual(count, 0)
        self.assertTrue(lines[0].startswith("NONE dub"), lines)

    def test_matches_an_exact_path_tag_ignoring_case_and_counts_each_file_once(self):
        # The firmware's isPath() ignores case; the folder's own tag reaches the same file.
        ok, lines, count = riser_fit.check(
            f"dub\t3000\t{RISERS}\tsamples/fx/riser/gen riser short.WAV|Short\n",
            self.pack,
        )
        self.assertTrue(ok, lines)
        self.assertEqual(count, 1)

    def test_reports_an_aiff_it_cant_measure(self):
        with open(os.path.join(self.pack, RISERS, "GEN Riser Aif.aif"), "wb") as f:
            f.write(b"FORM")
        ok, lines, _ = riser_fit.check(f"dub\t3000\t{RISERS}\tAif\n", self.pack)
        self.assertFalse(ok)
        self.assertTrue(
            lines[0].startswith("AIFF dub: SAMPLES/FX/Riser/GEN Riser Aif.aif"), lines
        )

    def test_refuses_a_listing_it_cant_read(self):
        with self.assertRaises(riser_fit.CantCheck):
            riser_fit.check("dub\t3000\n", self.pack)

    def test_holds_for_the_committed_pack_and_genre_tables(self):
        render = riser_fit.osc_octaves.find_render()
        if render is None:
            self.skipTest("no generator_render: run ./dbt test --generator")
        self.assertEqual(riser_fit.main(["--render", render]), 0)


if __name__ == "__main__":
    unittest.main()
