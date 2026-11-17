"""Tests for theory_review.py: the theory review's summary of generator_render's MIDI (spec §21.5).

python3 scripts/generative/test_theory_review.py
"""

import io
import os
import re
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import theory_review

BAR = theory_review.TICKS_PER_BAR
STEP = theory_review.TICKS_PER_STEP


def vlq(n):
    out = [n & 0x7F]
    n >>= 7
    while n:
        out.insert(0, (n & 0x7F) | 0x80)
        n >>= 7
    return bytes(out)


def track(events, end):
    """events: (tick, bytes) with bytes a whole MIDI or meta event, in any order."""
    body, last = b"", 0
    for tick, data in sorted(events, key=lambda e: e[0]):
        body += vlq(tick - last) + data
        last = tick
    body += vlq(max(end - last, 0)) + b"\xff\x2f\x00"
    return b"MTrk" + len(body).to_bytes(4, "big") + body


def meta(kind, text):
    data = text.encode()
    return b"\xff" + bytes([kind]) + vlq(len(data)) + data


def note(tick, key, velocity, channel=0, length=6):
    return [
        (tick, bytes([0x90 | channel, key, velocity])),
        (tick + length, bytes([0x80 | channel, key, 0x40])),
    ]


def smf(tracks):
    return (
        b"MThd"
        + bytes([0, 0, 0, 6, 0, 1])
        + len(tracks).to_bytes(2, "big")
        + bytes([0, 96])
        + b"".join(tracks)
    )


RECIPE = (
    "recipe genre=test-genre seed=1 entropy=1 tempo=128.00 swing=0 energy=25.0 density=25.0 complexity=25.0"
    " motion=25.0 phrase=8 hysteresis=2.0"
)


def tiny_piece_bytes(
    bars=4, lead_notes=(), extra_theory=(), chords=None, densities=None
):
    """A 4-bar piece: DRUMS (Kick on every beat, HatC on the & steps), BASS on each chord's root, LEAD gated at 30, A minor.
    Sections: GROOVE 1 bars 0-1, BREAK 1 bars 2-3."""
    end = bars * BAR
    conductor = track(
        [(0, meta(0x06, "GROOVE 1")), (2 * BAR, meta(0x06, "BREAK 1"))], end
    )
    drums_events = [(0, meta(0x03, "DRUMS"))]
    bass_events = [(0, meta(0x03, "BASS"))]
    for bar in range(bars):
        for beat in range(4):
            t = bar * BAR + beat * 4 * STEP
            drums_events += note(t, 36, 100, channel=9)
            drums_events += note(t + 2 * STEP, 42, 70, channel=9)
        bass_events += note(
            bar * BAR, 45 if bar < 2 else 50, 90, channel=1
        )  # the chord root
    lead_events = [(0, meta(0x03, "LEAD"))]
    for tick, key in lead_notes:
        lead_events += note(tick, key, 80, channel=2)
    theory = [
        RECIPE,
        "part DRUMS role=Drums flavour=None gate=0.0",
        "lane DRUMS.Kick gate=0.0 note=36",
        "lane DRUMS.HatC gate=0.0 note=42",
        "rule source=DRUMS.HatC kind=Lock target=DRUMS.Kick off=1 strength=90.0",
        "part BASS role=Bass flavour=Loop gate=0.0",
        "part LEAD role=Lead flavour=Motif gate=30.0",
        "key root=A scale=minor pcs=9,11,0,2,4,5,7",
    ]
    theory_events = [(0, meta(0x03, "THEORY"))] + [
        (0, meta(0x01, line)) for line in theory
    ]
    # i for two bars, then iv (steps 3,5,7) for two.
    for tick, steps in chords or [(0, "0,2,4"), (2 * BAR, "3,5,7")]:
        theory_events.append((tick, meta(0x01, f"chord {steps}")))
    for tick, density in densities or [(0, 25.0)]:
        theory_events.append(
            (tick, meta(0x01, f"macros energy=25.0 density={density}"))
        )
    theory_events += [(t, meta(0x01, line)) for t, line in extra_theory]
    return smf(
        [
            conductor,
            track(drums_events, end),
            track(bass_events, end),
            track(lead_events, end),
            track(theory_events, end),
        ]
    )


class ReadTest(unittest.TestCase):
    def test_reads_the_theory_track_notes_and_sections(self):
        piece = theory_review.piece_from_midi(tiny_piece_bytes())
        self.assertEqual(piece.recipe["genre"], "test-genre")
        self.assertEqual([p.name for p in piece.parts], ["DRUMS", "BASS", "LEAD"])
        self.assertEqual(piece.parts[0].lanes, {"Kick": 0.0, "HatC": 0.0})
        self.assertEqual(piece.parts[0].lane_notes, {36: "Kick", 42: "HatC"})
        self.assertEqual(piece.parts[2].gate, 30.0)
        self.assertEqual(len(piece.rules), 1)
        self.assertTrue(piece.rules[0].off)
        self.assertEqual(piece.bars, 4)
        self.assertEqual(piece.sections, [(0, "GROOVE 1"), (2, "BREAK 1")])
        self.assertEqual(piece.keys, [(0, 9, "minor", [9, 11, 0, 2, 4, 5, 7])])
        self.assertEqual(piece.chords, [(0, [0, 2, 4]), (2 * BAR, [3, 5, 7])])
        self.assertEqual(len(piece.parts[0].notes), 32)
        self.assertEqual(
            piece.onsets("DRUMS.Kick"), {s for s in range(64) if s % 4 == 0}
        )

    def test_refuses_a_file_without_a_theory_track(self):
        data = smf([track([(0, meta(0x06, "GROOVE 1"))], BAR)])
        with self.assertRaisesRegex(ValueError, "no THEORY track"):
            theory_review.piece_from_midi(data)

    def test_counts_a_note_on_the_nearest_16th(self):
        # A flam's grace hit 3 ticks early and a lean 2 ticks late both land on step 4.
        self.assertEqual(theory_review.Note(4 * STEP - 3, 36, 90).step, 4)
        self.assertEqual(theory_review.Note(4 * STEP + 2, 36, 90).step, 4)


class MusicTest(unittest.TestCase):
    A_MINOR = (0, 9, "minor", [9, 11, 0, 2, 4, 5, 7])
    A_DORIAN = (0, 9, "dorian", [9, 11, 0, 2, 4, 6, 7])

    def test_names_chords_by_roman_numeral_in_the_key(self):
        name = theory_review.chord_name
        self.assertEqual(name(self.A_MINOR, [0, 2, 4, 6]), "i")
        self.assertEqual(name(self.A_MINOR, [1, 3, 5]), "ii°")
        self.assertEqual(name(self.A_MINOR, [3, 5, 7]), "iv")
        self.assertEqual(name(self.A_MINOR, [5, 7, 9]), "bVI")
        self.assertEqual(name(self.A_MINOR, [6, 8, 10]), "bVII")
        self.assertEqual(name(self.A_MINOR, [-1, 1, 3]), "bVII")
        self.assertEqual(name(self.A_DORIAN, [3, 5, 7]), "IV")
        self.assertEqual(name(self.A_DORIAN, [5, 7, 9]), "vi°")

    def test_labels_pitch_classes_as_degrees_and_out_of_scale_as_none(self):
        label = theory_review.degree_label
        self.assertEqual(label(self.A_MINOR, 9), "1")
        self.assertEqual(label(self.A_MINOR, 0), "b3")
        self.assertEqual(label(self.A_DORIAN, 6), "6")
        self.assertIsNone(label(self.A_MINOR, 1))


class SummaryTest(unittest.TestCase):
    def summary(self, **kwargs):
        return theory_review.summary(
            [theory_review.piece_from_midi(tiny_piece_bytes(**kwargs))]
        )

    def grid_row(self, text, kind, source):
        block = text.split(f"  {kind} (")[1]
        row = next(
            line for line in block.splitlines() if line.strip().startswith(source)
        )
        return row.split()[1:]

    def test_shows_each_lane_s_onsets_by_16th_as_a_percent_of_the_section_kind_s_bars(
        self,
    ):
        text = self.summary()
        kick = self.grid_row(text, "GROOVE", "DRUMS.Kick")
        self.assertEqual(
            kick,
            [
                "100",
                ".",
                ".",
                ".",
                "100",
                ".",
                ".",
                ".",
                "100",
                ".",
                ".",
                ".",
                "100",
                ".",
                ".",
                ".",
                "8",
            ],
        )
        hat = self.grid_row(text, "BREAK", "DRUMS.HatC")
        self.assertEqual(hat[2], "100")
        self.assertEqual(hat[0], ".")

    def test_reports_the_chords_per_section_and_the_harmonic_rhythm(self):
        text = self.summary()
        self.assertIn("  GROOVE 1: ix2", text)
        self.assertIn("  BREAK 1: ivx2", text)
        self.assertIn(
            "Harmonic rhythm: 1 changes, spans 2 bars x1; 0 off a bar line", text
        )
        self.assertIn("Degree share of bars: i 50%, iv 50%", text)

    def test_flags_a_chord_change_off_a_bar_line(self):
        text = self.summary(chords=[(0, "0,2,4"), (BAR + 8 * STEP, "3,5,7")])
        self.assertIn("1 off a bar line (first at bar 1.50)", text)

    def test_reports_out_of_scale_notes_and_chord_tones(self):
        # A# (58) isn't in A minor; C (60) is the i chord's 3rd.
        text = self.summary(lead_notes=[(0, 58), (4 * STEP, 60)])
        line = next(
            l
            for l in text.splitlines()
            if l.startswith("  LEAD:") and "out of scale" in l
        )
        self.assertIn("b3:50%", line)
        self.assertIn("out of scale 1 (A#3@bar0(e1))", line)
        self.assertIn("on chord 50%", line)
        bass = next(
            l
            for l in text.splitlines()
            if l.startswith("  BASS:") and "out of scale" in l
        )
        self.assertIn("out of scale 0", bass)
        self.assertIn("on chord 100%", bass)

    def test_reports_ranges_and_their_overlaps(self):
        text = self.summary(lead_notes=[(0, 48), (BAR, 60)])
        self.assertIn("  BASS: A2-D3, median A2; velocity 90-90", text)
        self.assertIn("  LEAD: C3-C4, median C3; velocity 80-80", text)
        self.assertIn("Range overlaps: BASS/LEAD C3-D3 (2 st)", text)

    def test_reports_notes_per_bar_per_section(self):
        text = self.summary()
        row = next(l for l in text.splitlines() if l.startswith("  GROOVE 1 "))
        self.assertEqual(row.split()[2:6], ["8.0", "1.0", "0.0", "9->9"])

    def test_counts_a_gated_part_s_notes_by_where_density_stood_on_each_bar_line(self):
        # Density 25 for bars 0-2 (below 30 - 2), 29 on bar 3 (the band); LEAD plays on bar 0 and bar 3.
        text = self.summary(
            lead_notes=[(0, 57), (3 * BAR, 57)], densities=[(0, 25.0), (3 * BAR, 29.0)]
        )
        self.assertIn(
            "  LEAD gate 30: below 3 bars, 1 notes; band 1 bars, 1 notes; open 0 bars, 0 notes",
            text,
        )

    def test_measures_each_interplay_rule(self):
        text = self.summary()
        self.assertIn(
            "  DRUMS.HatC Lock off -> DRUMS.Kick at 90%: 16 onsets, on 0% / offbeat 100% / busier half 0%",
            text,
        )

    def test_reports_velocity_per_lane_and_part(self):
        text = self.summary()
        self.assertIn("  DRUMS.Kick: 100-100, median 100", text)
        self.assertIn("  DRUMS.HatC: 70-70, median 70", text)


class CommandTest(unittest.TestCase):
    def test_summarises_midi_files_given(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "a.mid")
            with open(path, "wb") as f:
                f.write(tiny_piece_bytes())
            out = io.StringIO()
            with redirect_stdout(out):
                theory_review.main(["--midi", path])
        self.assertTrue(
            out.getvalue().startswith("THEORY REVIEW: test-genre, 1 renders")
        )

    def test_renders_one_arc_cycle_per_entropy_with_the_seed_set_to_it(self):
        calls = []

        def run(cmd, **kwargs):
            calls.append(cmd)
            out = cmd[cmd.index("--out") + 1]
            with open(out + ".mid", "wb") as f:
                f.write(tiny_piece_bytes())
            return subprocess.CompletedProcess(cmd, 0, "", "")

        with (
            tempfile.TemporaryDirectory() as tmp,
            mock.patch.object(subprocess, "run", run),
            redirect_stdout(io.StringIO()),
        ):
            theory_review.main(
                [
                    "--genre",
                    "test-genre",
                    "--render",
                    "/bin/render",
                    "--out",
                    tmp,
                    "--entropies",
                    "4,5",
                    "--",
                    "--density",
                    "40",
                ]
            )
        self.assertEqual(len(calls), 2)
        self.assertEqual(
            calls[0],
            [
                "/bin/render",
                "--genre",
                "test-genre",
                "--entropy",
                "4",
                "--seed",
                "4",
                "--bars",
                "1000",
                "--end-after-drop",
                "--midi-only",
                "--out",
                os.path.join(tmp, "test-genre-e4"),
                "--density",
                "40",
            ],
        )
        self.assertIn("--entropy", calls[1])
        self.assertEqual(calls[1][calls[1].index("--entropy") + 1], "5")


RENDER = theory_review.find_render()


@unittest.skipUnless(RENDER, "no generator_render built: ./dbt test --generator -n")
class DetroitTest(unittest.TestCase):
    """The ticket's check on the real render (#312): a 2-bar harmonic rhythm, the kick on every beat, every note in
    scale, and the Motif silent below its gate. Skipped when the harness isn't built."""

    @classmethod
    def setUpClass(cls):
        with tempfile.TemporaryDirectory() as tmp:
            out = io.StringIO()
            with redirect_stdout(out):
                theory_review.main(["--genre", "techno-detroit", "--out", tmp])
            cls.text = out.getvalue()

    def test_two_bar_harmonic_rhythm(self):
        rhythms = re.findall(
            r"Harmonic rhythm: \d+ changes, spans (.*); (\d+) off a bar line", self.text
        )
        self.assertEqual(len(rhythms), 3)
        for spans, off in rhythms:
            self.assertEqual(off, "0")
            self.assertRegex(spans, r"^2 bars x\d+$")

    def test_kick_on_every_beat(self):
        for kind in ("INTRO", "GROOVE", "OUTRO"):
            block = self.text.split(f"  {kind} (")[1]
            row = next(
                line
                for line in block.splitlines()
                if line.strip().startswith("DRUMS.Kick")
            ).split()[1:17]
            for beat in (0, 4, 8, 12):
                self.assertGreaterEqual(
                    int(row[beat]), 95, f"{kind} kick on step {beat}: {row}"
                )

    def test_every_note_in_scale(self):
        lines = [l for l in self.text.splitlines() if "out of scale" in l]
        self.assertEqual(len(lines), 4)
        for line in lines:
            self.assertIn("out of scale 0;", line)

    def test_motif_absent_below_its_gate(self):
        gates = re.findall(r"MOTIF gate 30: below (\d+) bars, (\d+) notes", self.text)
        self.assertEqual(len(gates), 3)
        for bars, notes in gates:
            self.assertGreater(int(bars), 0)
            self.assertEqual(notes, "0")


if __name__ == "__main__":
    unittest.main()
