"""Unit tests for golden_song.py: python3 -m unittest discover -s scripts/generative -p 'test_*.py'"""

import os
import re
import unittest

import golden_song as g

REPO = g.REPO
CORE = os.path.join(REPO, "src", "deluge", "generative", "core")

HEADER = """# techno-acid's golden (spec 21.2): its default Recipe, seed 1, 16 bars from SHIFT+PLAY.
format 1
genre techno-acid version 2
recipe entropy 0: G minor, 125.00 BPM, swing 0, energy 25, density 25, complexity 25, motion 25
parts 0 DRUMS, 1 ACID, 2 DRONE, 3 FX
fingerprint 6efee21d65dc81f6, 1514111 events, 0 lost (60 pieces with random macros, scales, keys and seeds)

"""

RECIPE = """<?xml version="1.0" encoding="UTF-8"?>
<generativeRecipe genre="techno-acid" genreVersion="2" tempo="12500" swing="0" swingInterval="0" key="7"
\tscale="minor" seed="1">
\t<macros energy="1600" density="1600" complexity="1600" motion="1600" />
\t<part name="DRUMS" take="0" octave="0" />
\t<part name="ACID" take="0" octave="0" sound="SYNTHS/005 Sweet Mono Bass.XML" />
\t<part name="DRONE" take="0" octave="0" sound="SYNTHS/148 Warm 5th Pad.XML" />
\t<part name="FX" take="0" octave="0" />
</generativeRecipe>
"""


def golden(*lines):
    return (
        HEADER
        + "\n".join(g.event_line(t, owner, what) for t, owner, what in lines)
        + "\n"
    )


def note_data(*notes):
    """noteDataWithSplitProb for (position, length, velocity) notes: lift 64, probability 20, no iterance or fill."""
    return "0x" + "".join(f"{p:08X}{n:08X}{v:02X}4014000000" for p, n, v in notes)


def song(drums=(), acid=(), drone=()):
    """A Compose song's shape: DRUMS (a Kit: KICK, CLOSED HAT), ACID and DRONE synths; FX never sounds."""
    rows = lambda pairs, attr: "".join(
        f'<noteRow {attr}="{k}" noteDataWithSplitProb="{note_data(*v)}" />'
        for k, v in pairs
    )
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<song firmwareVersion="c1.3.0">
<instruments>
<kit presetName="DRUMS" presetFolder="KITS"><soundSources><sound name="KICK" /><sound name="CLOSED HAT" />
</soundSources></kit>
<sound presetName="005 Sweet Mono Bass" presetFolder="SYNTHS" />
<sound presetName="148 Warm 5th Pad" presetFolder="SYNTHS" />
</instruments>
<sessionClips>
<instrumentClip instrumentPresetName="DRUMS" length="6144"><noteRows>{rows(drums, "drumIndex")}
<noteRow drumIndex="1" /></noteRows></instrumentClip>
<instrumentClip instrumentPresetName="005 Sweet Mono Bass" length="6144"><noteRows>{rows(acid, "y")}</noteRows>
</instrumentClip>
<instrumentClip instrumentPresetName="148 Warm 5th Pad" length="6144"><noteRows>{rows(drone, "y")}</noteRows>
</instrumentClip>
</sessionClips>
</song>
"""


# A golden's notes, and the same song: a Kick, an ornament hat, a muted Acid note Compose drops, an Acid note, and a
# Drone note still sounding at the golden's last tick (its stop's note-off), longer in the song.
GOLDEN = golden(
    (0, "DRUMS", "Kick on v88"),
    (0, "DRONE", "G3 on v88"),
    (6, "DRUMS", "Kick off"),
    (24, "DRUMS", "HatC on v70 ornament"),
    (30, "DRUMS", "HatC off"),
    (48, "ACID", "F2 on v84 muted"),
    (48, "ACID", "Brightness -16"),
    (64, "ACID", "F2 off"),
    (96, "ACID", "G1 on v100"),
    (108, "ACID", "G1 off"),
    (6000, "DRUMS", "Kick on v90"),
    (6006, "DRUMS", "Kick off"),
    (6132, "DRONE", "G3 off"),
)
SONG = song(drums=[(0, [(0, 6, 88), (6000, 6, 90), (6130, 6, 90)]), (1, [(24, 6, 70)])],
            acid=[(31, [(96, 12, 100)])], drone=[(55, [(0, 6144, 88)])])  # fmt: skip


class CoreTables(unittest.TestCase):
    """The module's copies of the core's grid and names match the C++ they mirror."""

    def constant(self, path, name):
        with open(path) as fh:
            return int(
                re.search(rf"constexpr int32_t {name} = (\d+);", fh.read()).group(1)
            )

    def strings(self, path, start):
        with open(path) as fh:
            text = fh.read()
        body = text[text.index(start) :]
        return tuple(re.findall(r'"([^"]*)"', body[: body.index("}")]))

    def test_the_grid(self):
        types = os.path.join(CORE, "types.h")
        self.assertEqual(g.TICKS_PER_STEP, self.constant(types, "kTicksPerStep"))
        self.assertEqual(g.STEPS_PER_BAR, self.constant(types, "kStepsPerBar"))
        self.assertEqual(g.MAX_LEAN, self.constant(types, "kMaxLean"))
        recipe = os.path.join(CORE, "recipe.h")
        self.assertEqual(
            g.MACRO_UNITS_PER_POINT, self.constant(recipe, "kMacroUnitsPerPoint")
        )

    def test_the_lane_names(self):
        self.assertEqual(
            g.SONG_LANES,
            self.strings(os.path.join(CORE, "lane_names.h"), "kNames[kNumLanes]"),
        )
        names = os.path.join(REPO, "tests", "spec", "generator", "render", "names.cpp")
        self.assertEqual(g.GOLDEN_LANES, self.strings(names, "kLaneNames[kNumLanes]"))
        self.assertEqual(g.PITCH_NAMES, self.strings(names, "kPitchNames[12]"))


class GoldenText(unittest.TestCase):
    def test_every_line_of_a_committed_golden_is_written_back_the_same(self):
        with open(g.golden_path("techno-acid")) as fh:
            lines = [line for line in fh.read().splitlines() if line.startswith(" ")]
        self.assertGreater(len(lines), 1000)
        for line in lines:
            tick, _, owner, what = g.EVENT_LINE.match(line).groups()
            self.assertEqual(g.event_line(int(tick), owner, what), line)

    def test_note_names(self):
        self.assertEqual(
            [g.note_name(p) for p in (31, 41, 0, 127)], ["G1", "F2", "C-1", "G9"]
        )

    def test_the_header_and_the_notes_paired_without_the_muted_one(self):
        parsed = g.Golden(GOLDEN)
        self.assertEqual(
            (parsed.genre, parsed.version, parsed.seed), ("techno-acid", 2, 1)
        )
        self.assertEqual(parsed.parts, ["DRUMS", "ACID", "DRONE", "FX"])
        self.assertEqual(parsed.last_tick, 6132)
        self.assertEqual(
            sorted(parsed.notes),
            [(0, 6, "DRUMS", "Kick", 88), (0, 6132, "DRONE", "G3", 88), (24, 6, "DRUMS", "HatC", 70),
             (96, 12, "ACID", "G1", 100), (6000, 6, "DRUMS", "Kick", 90)],
        )  # fmt: skip

    def test_a_note_off_ends_the_earliest_open_note_of_its_key(self):
        parsed = g.Golden(
            golden(
                (0, "ACID", "G1 on v1"),
                (12, "ACID", "G1 on v2"),
                (24, "ACID", "G1 off"),
            )
        )
        self.assertEqual(parsed.notes, [(0, 24, "ACID", "G1", 1)])

    def test_a_file_with_no_header_is_refused(self):
        with self.assertRaises(g.GoldenError):
            g.Golden("      0  1.1.1     DRUMS    Kick on v88\n")


class SongNotes(unittest.TestCase):
    def test_note_data_reads_each_note(self):
        self.assertEqual(
            g.read_note_data(note_data((0, 6, 88), (0x60, 0x18, 127))),
            [(0, 6, 88), (96, 24, 127)],
        )
        with self.assertRaises(g.GoldenError):
            g.read_note_data("0x00000000")

    def test_a_note_that_doesnt_play_as_compose_writes_it_is_refused(self):
        good = note_data((0, 6, 88))
        for name, tail in (
            ("lift", "3F14000000"),
            ("probability", "4013000000"),
            ("iterance", "4014000100"),
            ("fill", "4014000001"),
        ):
            with (
                self.subTest(name),
                self.assertRaisesRegex(g.GoldenError, "not Compose's"),
            ):
                g.read_note_data(good[: -len(tail)] + tail)
        # the second note's tail is checked too
        with self.assertRaisesRegex(g.GoldenError, "note at 96"):
            g.read_note_data(good + "00000060" + "00000006" + "58" + "4013000000")

    def test_rows_named_as_the_golden_names_them(self):
        sounds, _ = g.recipe_sounds(RECIPE)
        self.assertEqual(
            sounds, {"ACID": "005 Sweet Mono Bass", "DRONE": "148 Warm 5th Pad"}
        )
        notes = g.song_notes(SONG, ["DRUMS", "ACID", "DRONE", "FX"], sounds)
        self.assertIn((24, 6, "DRUMS", "HatC", 70), notes)
        self.assertIn((96, 12, "ACID", "G1", 100), notes)
        self.assertIn((0, 6144, "DRONE", "G3", 88), notes)

    def test_an_instrument_no_part_owns_is_refused(self):
        with self.assertRaisesRegex(g.GoldenError, "no part"):
            g.song_notes(
                SONG,
                ["DRUMS", "ACID", "DRONE", "FX"],
                {"ACID": "Other", "DRONE": "148 Warm 5th Pad"},
            )


class Compare(unittest.TestCase):
    def test_the_same_notes_pass(self):
        problems, song, expected = g.compare(GOLDEN, SONG, RECIPE)
        self.assertEqual(problems, [])
        self.assertEqual(song, expected)
        # the note too close to the golden's end to be compared is left out; the Drone is cut at the last tick
        self.assertNotIn(g.event_line(6130, "DRUMS", "Kick on v90"), song)
        self.assertIn(g.event_line(6132, "DRONE", "G3 off"), song)

    def test_a_changed_velocity_length_or_pitch_differs(self):
        for changed in (
            song(drums=[(0, [(0, 6, 89), (6000, 6, 90)]), (1, [(24, 6, 70)])], acid=[(31, [(96, 12, 100)])],
                 drone=[(55, [(0, 6144, 88)])]),
            song(drums=[(0, [(0, 6, 88), (6000, 6, 90)]), (1, [(24, 6, 70)])], acid=[(31, [(96, 13, 100)])],
                 drone=[(55, [(0, 6144, 88)])]),
            song(drums=[(0, [(0, 6, 88), (6000, 6, 90)]), (1, [(24, 6, 70)])], acid=[(32, [(96, 12, 100)])],
                 drone=[(55, [(0, 6144, 88)])]),
        ):  # fmt: skip
            _, song_lines, golden_lines = g.compare(GOLDEN, changed, RECIPE)
            self.assertNotEqual(song_lines, golden_lines)

    def test_a_missing_note_differs(self):
        changed = song(drums=[(0, [(0, 6, 88), (6000, 6, 90)])], acid=[(31, [(96, 12, 100)])],
                       drone=[(55, [(0, 6144, 88)])])  # fmt: skip
        _, song_lines, golden_lines = g.compare(GOLDEN, changed, RECIPE)
        self.assertNotEqual(song_lines, golden_lines)

    def test_a_recipe_file_off_the_golden_case_is_named(self):
        changed = RECIPE.replace('seed="1"', 'seed="2"').replace(
            'energy="1600"', 'energy="1664"'
        )
        changed = changed.replace('name="ACID" take="0"', 'name="ACID" take="3"')
        problems, _, _ = g.compare(GOLDEN, SONG, changed)
        self.assertEqual(len(problems), 3)
        self.assertTrue(problems[0].startswith("seed='2'"))


if __name__ == "__main__":
    unittest.main()
