"""Tests for osc_octaves.py: palette presets whose oscillators all sit an octave or more off the note (spec §10.6).

python3 scripts/generative/test_osc_octaves.py
"""

import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import osc_octaves


def preset(osc1, osc2, mode="subtractive", whole=None, b_volume="0x7FFFFFFF"):
    """A preset as the factory card writes them: the firmware version tags, then <sound>."""
    whole_tag = f"<transpose>{whole}</transpose>" if whole is not None else ""
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<firmwareVersion>2.0.0-beta</firmwareVersion>
<earliestCompatibleFirmware>2.0.0-beta</earliestCompatibleFirmware>
<sound>
	<osc1><type>saw</type><transpose>{osc1}</transpose><cents>0</cents></osc1>
	<osc2><type>square</type><transpose>{osc2}</transpose><cents>0</cents></osc2>
	<mode>{mode}</mode>
	{whole_tag}
	<defaultParams>
		<oscAVolume>0x7FFFFFFF</oscAVolume>
		<oscBVolume>{b_volume}</oscBVolume>
	</defaultParams>
</sound>
"""


class Oscillators(unittest.TestCase):
    def test_flags_a_preset_whose_oscillators_all_carry_an_octave(self):
        self.assertTrue(osc_octaves.off_octave(osc_octaves.parse(preset(-36, -24))))
        self.assertTrue(osc_octaves.off_octave(osc_octaves.parse(preset(12, 24))))

    def test_passes_one_oscillator_at_the_note_or_within_the_octave(self):
        self.assertFalse(osc_octaves.off_octave(osc_octaves.parse(preset(0, -24))))
        self.assertFalse(osc_octaves.off_octave(osc_octaves.parse(preset(-5, -12))))
        self.assertFalse(osc_octaves.off_octave(osc_octaves.parse(preset(-11, 7))))

    def test_ignores_the_whole_sound_transpose_which_the_load_sets_to_0(self):
        self.assertFalse(
            osc_octaves.off_octave(osc_octaves.parse(preset(0, -12, whole=-24)))
        )

    def test_counts_only_the_oscillators_that_sound(self):
        # osc2 is at the note but its volume is off: only osc1, two octaves down, is heard
        silent_b = preset(-24, 0, b_volume="0x80000000")
        self.assertTrue(osc_octaves.off_octave(osc_octaves.parse(silent_b)))
        # Ring mod multiplies both oscillators, whatever their volumes
        self.assertFalse(
            osc_octaves.off_octave(
                osc_octaves.parse(preset(-24, 0, mode="ringmod", b_volume="0x80000000"))
            )
        )

    def test_reads_old_presets_attributes(self):
        old = """<?xml version="1.0" encoding="UTF-8"?>
<sound firmwareVersion="1.2.0" mode="subtractive">
	<osc1 type="saw" transpose="-24" />
	<osc2 type="saw" transpose="-12" />
	<defaultParams oscAVolume="0x7FFFFFFF" oscBVolume="0x7FFFFFFF" />
</sound>
"""
        self.assertTrue(osc_octaves.off_octave(osc_octaves.parse(old)))


class Check(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.card = os.path.join(self.tmp.name, "card")
        self.pack = os.path.join(self.tmp.name, "pack")
        for root, name, text in [
            (self.card, "SYNTHS/001 Low.XML", preset(-24, -12)),
            (self.card, "SYNTHS/002 Fine.XML", preset(0, -12, whole=-24)),
            (self.pack, "SYNTHS/GENERATIVE/003 Pack Low.XML", preset(-12, -12)),
        ]:
            path = os.path.join(root, name)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w") as fh:
                fh.write(text)

    def tearDown(self):
        self.tmp.cleanup()

    def run_check(self, listing, allow):
        return osc_octaves.check(listing, [self.pack, self.card], allow)

    def test_flags_a_listed_preset_on_the_card_or_in_the_pack(self):
        listing = (
            "techno-dub\tSUB\tSYNTHS/001 Low.XML\n"
            "techno-dub\tSUB\tSYNTHS/002 Fine.XML\n"
            "techno-dub\tPAD\tSYNTHS/GENERATIVE/003 Pack Low.XML\n"
        )
        ok, lines, _ = self.run_check(listing, {})
        self.assertFalse(ok)
        flagged = [line for line in lines if line.startswith("FLAGGED")]
        self.assertEqual(len(flagged), 2)
        self.assertIn("techno-dub SUB SYNTHS/001 Low.XML", flagged[0])
        self.assertIn("osc -24 -12", flagged[0])

    def test_an_allowlisted_preset_passes_with_its_reason(self):
        ok, lines, _ = self.run_check(
            "techno-acid\tACID\tSYNTHS/001 Low.XML\n",
            {"SYNTHS/001 Low.XML": "its Register is set for it"},
        )
        self.assertTrue(ok)
        self.assertTrue(
            any(
                "allowed" in line and "its Register is set for it" in line
                for line in lines
            )
        )

    def test_an_allowlist_entry_no_palette_lists_fails_so_the_list_stays_short(self):
        ok, lines, _ = self.run_check(
            "techno-dub\tSUB\tSYNTHS/002 Fine.XML\n",
            {"SYNTHS/001 Low.XML": "once needed"},
        )
        self.assertFalse(ok)
        self.assertTrue(any("no palette lists" in line for line in lines))

    def test_an_allowlisted_preset_no_longer_flagged_is_stale(self):
        ok, lines, _ = self.run_check(
            "techno-dub\tSUB\tSYNTHS/002 Fine.XML\n",
            {"SYNTHS/002 Fine.XML": "once off"},
        )
        self.assertFalse(ok)
        self.assertTrue(any("STALE" in line and "no longer" in line for line in lines))

    def test_counts_each_preset_once(self):
        listing = "a\tX\tSYNTHS/002 Fine.XML\nb\tY\tSYNTHS/002 Fine.XML\n"
        self.assertEqual(self.run_check(listing, {})[2], 1)

    def test_an_unreadable_preset_cant_be_checked(self):
        with open(os.path.join(self.card, "SYNTHS/005 Bad.XML"), "w") as fh:
            fh.write("<sound><osc1>")
        with self.assertRaises(osc_octaves.CantCheck):
            self.run_check("techno-dub\tSUB\tSYNTHS/005 Bad.XML\n", {})

    def test_a_preset_on_neither_root_fails(self):
        ok, lines, _ = self.run_check("techno-dub\tSUB\tSYNTHS/404 Gone.XML\n", {})
        self.assertFalse(ok)
        self.assertTrue(any("MISSING" in line for line in lines))

    def test_the_repo_allowlist_reasons_are_written(self):
        for path, reason in osc_octaves.ALLOWED.items():
            self.assertTrue(path.startswith("SYNTHS/"), path)
            self.assertGreater(len(reason), 20, path)


if __name__ == "__main__":
    unittest.main()
