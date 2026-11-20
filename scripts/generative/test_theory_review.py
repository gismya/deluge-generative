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
    bars=4,
    lead_notes=(),
    extra_theory=(),
    chords=None,
    densities=None,
    kick_bars=None,
    pad_notes=(),
    hat_notes=None,
    ornaments=(),
    sections=("GROOVE 1", "BREAK 1"),
):
    """A 4-bar piece: DRUMS (Kick on every beat of kick_bars, all by default; HatC on every & step), BASS on each
    chord's root, LEAD gated at 30, PAD (pad_notes: tick, key, length), A minor. Sections (two labels): bars 0-1 and
    2-3. hat_notes (ticks) replace the & hats; ornaments are THEORY's (tick, part, note) Ornament hits."""
    end = bars * BAR
    conductor = track(
        [(0, meta(0x06, sections[0])), (2 * BAR, meta(0x06, sections[1]))], end
    )
    drums_events = [(0, meta(0x03, "DRUMS"))]
    bass_events = [(0, meta(0x03, "BASS"))]
    for bar in range(bars):
        for beat in range(4):
            t = bar * BAR + beat * 4 * STEP
            if kick_bars is None or bar in kick_bars:
                drums_events += note(t, 36, 100, channel=9)
            if hat_notes is None:
                drums_events += note(t + 2 * STEP, 42, 70, channel=9)
        bass_events += note(
            bar * BAR, 45 if bar < 2 else 50, 90, channel=1
        )  # the chord root
    for tick in hat_notes or ():
        drums_events += note(tick, 42, 70, channel=9, length=4)
    lead_events = [(0, meta(0x03, "LEAD"))]
    for tick, key in lead_notes:
        lead_events += note(tick, key, 80, channel=2)
    pad_events = [(0, meta(0x03, "PAD"))]
    for tick, key, length in pad_notes:
        pad_events += note(tick, key, 70, channel=3, length=length)
    theory = [
        RECIPE,
        "part DRUMS role=Drums flavour=None gate=0.0",
        "lane DRUMS.Kick gate=0.0 note=36",
        "lane DRUMS.HatC gate=0.0 note=42",
        "rule source=DRUMS.HatC kind=Lock target=DRUMS.Kick off=1 strength=90.0",
        "part BASS role=Bass flavour=Loop gate=0.0",
        "part LEAD role=Lead flavour=Motif gate=30.0",
        "part PAD role=Pad flavour=Sustain gate=0.0",
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
    for t, part, key, *step in ornaments:  # (tick, part, note[, step that decided it])
        line = f"ornament part={part} note={key}" + (f" step={step[0]}" if step else "")
        theory_events.append((t, meta(0x01, line)))
    return smf(
        [
            conductor,
            track(drums_events, end),
            track(bass_events, end),
            track(lead_events, end),
            track(pad_events, end),
            track(theory_events, end),
        ]
    )


class ReadTest(unittest.TestCase):
    def test_reads_the_theory_track_notes_and_sections(self):
        piece = theory_review.piece_from_midi(tiny_piece_bytes())
        self.assertEqual(piece.recipe["genre"], "test-genre")
        self.assertEqual(
            [p.name for p in piece.parts], ["DRUMS", "BASS", "LEAD", "PAD"]
        )
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
    A_MINOR = theory_review.Key(0, 9, "minor", [9, 11, 0, 2, 4, 5, 7])
    A_DORIAN = theory_review.Key(0, 9, "dorian", [9, 11, 0, 2, 4, 6, 7])

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

    def test_names_chords_of_a_scale_not_7_notes_by_their_root_s_interval(self):
        # A pentatonic minor (A C D E G): no major-scale degree to sit against, so the numeral is the root's interval.
        pentatonic = theory_review.Key(0, 9, "pentatonic-minor", [9, 0, 2, 4, 7])
        self.assertEqual(theory_review.chord_name(pentatonic, [1, 3, 5]), "bIII")
        self.assertEqual(theory_review.chord_name(pentatonic, [3, 5, 7]), "v")

    def test_labels_and_orders_degrees_of_any_scale_size(self):
        # Every core scale has 7 notes today; a 5- or 6-note scale (a pentatonic, a blues scale) reads by interval.
        label, order = theory_review.degree_label, theory_review.degree_order
        sizes = {
            5: theory_review.Key(0, 9, "pentatonic-minor", [9, 0, 2, 4, 7]),
            6: theory_review.Key(0, 9, "blues", [9, 0, 2, 3, 4, 7]),
            7: theory_review.Key(0, 9, "locrian", [9, 10, 0, 2, 3, 5, 7]),
        }
        expected = {
            5: ["1", "b3", "4", "5", "b7"],
            6: ["1", "b3", "4", "#4", "5", "b7"],
            7: ["1", "b2", "b3", "4", "b5", "b6", "b7"],
        }
        for size, key in sizes.items():
            named = sorted(
                ((label(key, pc), pc) for pc in reversed(key.pcs)),
                key=lambda item: order(key, item),
            )
            self.assertEqual([n for n, _ in named], expected[size], size)
            name = theory_review.chord_name(
                key, [0, 2, 4]
            )  # never crashes, whatever the size
            self.assertIsInstance(name, str)

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
            "Harmonic rhythm: 1 changes, spans 2 bars x1; last 2 bars to the end; 0 off a bar line",
            text,
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

    def test_shows_the_pitch_classes_the_chord_and_pad_parts_sound_in_each_bar(self):
        # A minor triad held over bars 0-1 (one strike), then D F A from bar 2's line; nothing in bar 3's second half
        # changes what bar 3 sounds.
        held = 2 * BAR
        pad = [
            (0, 57, held),
            (0, 60, held),
            (0, 64, held),
            (2 * BAR, 50, 2 * BAR),
            (2 * BAR, 53, BAR),
            (2 * BAR, 57, BAR),
        ]
        text = self.summary(pad_notes=pad)
        self.assertIn("  GROOVE 1: [1 b3 5]x2", text)
        self.assertIn("  BREAK 1: [1 4 b6]x1 [4]x1", text)
        self.assertIn(
            "Heard harmonic rhythm (from bar 0): 2 changes, spans 1 bars x1, 2 bars x1; last 1 bars to the end",
            text,
        )

    def test_starts_the_heard_rhythm_where_the_chord_and_pad_parts_first_sound(self):
        # Silent for bar 0 (an Intro's gate, say): i in bar 1, then iv to the end.
        pad = [(BAR, 57, BAR), (2 * BAR, 50, 2 * BAR)]
        text = self.summary(pad_notes=pad)
        self.assertIn(
            "Heard harmonic rhythm (from bar 1): 1 changes, spans 1 bars x1; last 2 bars to the end",
            text,
        )
        self.assertIn(
            "Heard harmonic rhythm: the Chords and Pad parts never sound",
            self.summary(),
        )

    def hat_steps(self, hats, ornaments):
        piece = theory_review.piece_from_midi(
            tiny_piece_bytes(hat_notes=hats, ornaments=ornaments)
        )
        return piece, sorted(n.step for n in piece.part("DRUMS").notes if n.key == 42)

    def test_counts_a_ratchet_and_a_flam_once_on_the_step_that_decided_them(self):
        # A ratchet-3 leaned -2 on step 2 (hits at 46, 54, 62: the last rounds to step 3) and a flam on step 6 (grace
        # 3 ticks early). THEORY marks the later ratchet hits and the grace as Ornament hits, with their step.
        hats = [46, 54, 62, 6 * STEP - 3, 6 * STEP]
        ornaments = [
            (54, "DRUMS", 42, 2),
            (62, "DRUMS", 42, 2),
            (6 * STEP - 3, "DRUMS", 42, 6),
        ]
        piece, steps = self.hat_steps(hats, ornaments)
        self.assertEqual(piece.onsets("DRUMS.HatC"), {2, 6})
        self.assertEqual(steps, [2, 2, 2, 6, 6])

    def test_keeps_a_ratchet_off_the_next_16th_s_hit_on_the_same_key(self):
        # A ratchet-3 on step 2, then a plain hit on step 3: the ratchet's last hit (62) sits 10 ticks before it.
        _, steps = self.hat_steps(
            [48, 56, 62, 72], [(56, "DRUMS", 42, 2), (62, "DRUMS", 42, 2)]
        )
        self.assertEqual(steps, [2, 2, 2, 3])

    def test_keeps_a_ratchet_on_a_bar_s_last_16th_in_its_bar(self):
        # A ratchet-3 on bar 0's last 16th (step 15), then beat 1 of bar 1 leaned 2 early.
        last = 15 * STEP
        piece, steps = self.hat_steps(
            [last, last + 7, last + 14, BAR - 2],
            [(last + 7, "DRUMS", 42, 15), (last + 14, "DRUMS", 42, 15)],
        )
        self.assertEqual(steps, [15, 15, 15, 16])
        self.assertEqual(
            [n.bar for n in piece.part("DRUMS").notes if n.key == 42], [0, 0, 0, 1]
        )

    def test_folds_an_ornament_line_without_a_step_onto_the_main_hit_before_it(self):
        # An older render's lines: a ratchet's later hit goes to the last main hit on its key, a grace before every
        # main hit to the one after it.
        _, steps = self.hat_steps([45, 48, 56], [(45, "DRUMS", 42), (56, "DRUMS", 42)])
        self.assertEqual(steps, [2, 2, 2])

    def test_folds_a_grace_without_a_step_onto_the_main_hit_a_16th_after_it(self):
        # An older render's flam on step 6, after a plain hit on step 2: the grace is its next main hit's, not the
        # last one before it.
        _, steps = self.hat_steps(
            [2 * STEP, 6 * STEP - 3, 6 * STEP], [(6 * STEP - 3, "DRUMS", 42)]
        )
        self.assertEqual(steps, [2, 6, 6])

    def test_puts_a_note_leaned_early_onto_its_step_s_bar(self):
        # Bar 2's downbeat leaned 2 ticks early is bar 2's, in the notes per bar and the pitch classes.
        piece = theory_review.piece_from_midi(
            tiny_piece_bytes(lead_notes=[(2 * BAR - 2, 62)])
        )
        lead = piece.part("LEAD").notes[0]
        self.assertEqual((lead.step, lead.bar), (32, 2))
        text = theory_review.summary([piece])
        self.assertIn(
            "on chord 100%",
            next(
                l for l in text.splitlines() if l.startswith("  LEAD:") and "chord" in l
            ),
        )

    def test_shows_bars_where_the_chord_and_pad_parts_are_silent_as_empty(self):
        self.assertIn("  GROOVE 1: []x2", self.summary())

    def test_reports_ranges_and_their_overlaps(self):
        text = self.summary(lead_notes=[(0, 48), (BAR, 60)])
        self.assertIn("  BASS: A2-D3, median A2; velocity 90-90", text)
        self.assertIn("  LEAD: C3-C4, median C3; velocity 80-80", text)
        self.assertIn("Range overlaps: BASS/LEAD C3-D3 (2 st)", text)

    def test_reports_notes_per_bar_per_section(self):
        text = self.summary()
        row = next(l for l in text.splitlines() if l.startswith("  GROOVE 1 "))
        self.assertEqual(row.split()[2:7], ["8.0", "1.0", "0.0", "0.0", "9->9"])

    def test_files_density_at_gate_minus_hysteresis_as_below_where_every_gate_is_closed(
        self,
    ):
        text = self.summary(densities=[(0, 28.0), (BAR, 28.015625), (2 * BAR, 30.0)])
        self.assertIn(
            "  LEAD gate 30: below 1 bars, 0 notes; band 1 bars, 0 notes; open 2 bars, 0 notes",
            text,
        )

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
            "  DRUMS.HatC Lock off -> DRUMS.Kick at 90%: 16 of 16 onsets in bars the target plays: on 0% / offbeat 100%"
            " / against half 50%; all bars: on 0% / offbeat 100%",
            text,
        )

    def test_prints_each_render_s_strength_when_they_differ(self):
        # --midi files rendered at another Complexity: the pooled figures come from rules at 90 % and 70 %.
        first, second = (
            theory_review.piece_from_midi(tiny_piece_bytes()) for _ in range(2)
        )
        second.rules[0].strength = 70.0
        text = theory_review.summary([first, second])
        self.assertIn(
            "  DRUMS.HatC Lock off -> DRUMS.Kick at 90/70% by render: 32 of 32 onsets",
            text,
        )
        self.assertIn("at 90%: 16 of 16", self.summary())

    def test_pairs_each_render_s_strength_by_rule_not_by_its_place_in_the_list(self):
        # A render whose rules list another one first: its HatC rule is still at 90 %.
        first, second = (
            theory_review.piece_from_midi(tiny_piece_bytes()) for _ in range(2)
        )
        second.rules.insert(
            0, theory_review.Rule("DRUMS.Kick", "Avoid", "DRUMS.HatC", False, 10.0)
        )
        text = theory_review.summary([first, second])
        self.assertIn(
            "  DRUMS.HatC Lock off -> DRUMS.Kick at 90%: 32 of 32 onsets", text
        )

    def test_measures_a_rule_over_the_bars_its_target_plays(self):
        # The Kick sits out bars 2 and 3, as in a Break: the hats there have nothing to lock to.
        text = self.summary(kick_bars={0, 1})
        self.assertIn(
            "  DRUMS.HatC Lock off -> DRUMS.Kick at 90%: 8 of 16 onsets in bars the target plays: on 0% / offbeat 100%"
            " / against half 50%; all bars: on 0% / offbeat 50%",
            text,
        )

    def test_measures_a_respond_rule_by_the_target_s_against_half(self):
        # The hats play two & steps in each half of every bar: a tie, so each bar's first half is against.
        respond = (
            0,
            "rule source=LEAD kind=Respond target=DRUMS.HatC off=0 strength=100.0",
        )
        second_half = [
            (bar * BAR + step * STEP, 69) for bar in range(4) for step in (9, 11, 13)
        ]
        text = self.summary(extra_theory=[respond], lead_notes=second_half)
        self.assertIn(
            "  LEAD Respond -> DRUMS.HatC at 100%: 12 of 12 onsets in bars the target plays: on 0% / offbeat 0%"
            " / against half 0%",
            text,
        )
        first_half = [
            (bar * BAR + step * STEP, 69) for bar in range(4) for step in (1, 3)
        ]
        text = self.summary(extra_theory=[respond], lead_notes=first_half)
        self.assertIn(
            "8 of 8 onsets in bars the target plays: on 0% / offbeat 0% / against half 100%",
            text,
        )
        # With the hats only in a bar's second half, that half is against and the first is free.
        hats = [bar * BAR + step * STEP for bar in range(4) for step in (10, 14)]
        text = self.summary(
            extra_theory=[respond], lead_notes=first_half, hat_notes=hats
        )
        self.assertIn(
            "8 of 8 onsets in bars the target plays: on 0% / offbeat 0% / against half 0%",
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
                f.write(tiny_piece_bytes(sections=("GROOVE 1", "OUTRO 1")))
            out = io.StringIO()
            with redirect_stdout(out):
                theory_review.main(["--midi", path])
        self.assertTrue(
            out.getvalue().startswith("THEORY REVIEW: test-genre, 1 renders")
        )

    def fake_render(self, sections):
        def run(cmd, **kwargs):
            with open(cmd[cmd.index("--out") + 1] + ".mid", "wb") as f:
                f.write(tiny_piece_bytes(sections=sections))
            return subprocess.CompletedProcess(cmd, 0, "", "")

        return run

    def test_fails_a_render_that_never_reached_its_outro(self):
        # No Drop in --bars: generator_render warns, End was never armed and the piece ends in a Groove.
        with (
            tempfile.TemporaryDirectory() as tmp,
            mock.patch.object(
                subprocess, "run", self.fake_render(("GROOVE 1", "BREAK 1"))
            ),
            redirect_stdout(io.StringIO()),
            self.assertRaisesRegex(SystemExit, "no Outro at its end.*raise --bars"),
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
                    "1",
                ]
            )

    def test_summarises_a_midi_file_without_an_outro_but_says_so(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "a.mid")
            with open(path, "wb") as f:
                f.write(tiny_piece_bytes())
            err = io.StringIO()
            with redirect_stdout(io.StringIO()), mock.patch.object(sys, "stderr", err):
                theory_review.main(["--midi", path])
        self.assertIn(
            "no Outro at its end, so it isn't one Arc cycle; summarising it anyway",
            err.getvalue(),
        )

    def test_renders_one_arc_cycle_per_entropy_with_the_seed_set_to_it(self):
        calls = []

        def run(cmd, **kwargs):
            calls.append(cmd)
            out = cmd[cmd.index("--out") + 1]
            with open(out + ".mid", "wb") as f:
                f.write(tiny_piece_bytes(sections=("GROOVE 1", "OUTRO 1")))
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
    """The ticket's checks on the real render (#312), from the notes: a 2-bar harmonic rhythm heard, the kick on every
    beat, every note in scale. The Motif's gate is detroit_techno_spec's (from the notes too). Skipped when the
    harness isn't built."""

    @classmethod
    def setUpClass(cls):
        with tempfile.TemporaryDirectory() as tmp:
            out = io.StringIO()
            with redirect_stdout(out):
                theory_review.main(["--genre", "techno-detroit", "--out", tmp])
            cls.text = out.getvalue()

    def test_two_bar_harmonic_rhythm_heard(self):
        # From the notes the Strings and the Stab sound, not the Generator's chord state.
        rhythms = re.findall(
            r"Heard harmonic rhythm \(from bar \d+\): \d+ changes, spans (.*)",
            self.text,
        )
        self.assertEqual(len(rhythms), 3)
        for spans in rhythms:
            self.assertRegex(spans, r"^2 bars x\d+; last 2 bars to the end$")

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
        counts = re.findall(r"; out of scale (\d+)", self.text)
        self.assertEqual(counts, ["0"] * 4)
        self.assertNotIn(
            "!",
            "".join(
                l for l in self.text.splitlines() if l.startswith("  ") and "]x" in l
            ),
        )


if __name__ == "__main__":
    unittest.main()
