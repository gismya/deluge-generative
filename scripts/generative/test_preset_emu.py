"""Tests for preset_emu.py's lines and its audio measure, without an emulator.

The emulator run itself is preset_emu.py, which check_all.py runs (ICOUNT_CHECKS).

    python3 scripts/generative/test_preset_emu.py
"""

import math
import os
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gen_emu
import preset_emu

RATE = gen_emu.RATE


def tone(seconds, amplitude):
    """S32LE stereo: a 220 Hz sine at `amplitude` of full scale."""
    frames = int(seconds * RATE)
    out = bytearray()
    for i in range(frames):
        v = int(amplitude * (2**31 - 1) * math.sin(2 * math.pi * 220 * i / RATE))
        out += struct.pack("<ii", v, v)
    return bytes(out)


class NoteLines(unittest.TestCase):
    def test_a_note_is_quiet_then_on_then_off_with_its_waits(self):
        self.assertEqual(
            preset_emu.note_lines(
                0x9F, 0x8F, 0x3C, held_ms=1000, quiet_ms=500, release_ms=1500
            ),
            ["wait 500", "midi 9f 3c 64", "wait 1000", "midi 8f 3c 40", "wait 1500"],
        )

    def test_the_bytes_are_two_hex_digits_as_the_test_port_reads_them(self):
        lines = preset_emu.note_lines(0x90, 0x80, 0x05)
        self.assertIn("midi 90 05 64", lines)
        self.assertIn("midi 80 05 40", lines)

    def test_the_refused_lines_are_neither_notes_nor_loads_the_port_takes(self):
        for line in preset_emu.REFUSED:
            with self.subTest(line=line):
                self.assertTrue(line.startswith(("midi", "load ")))


class Rms(unittest.TestCase):
    def test_a_sine_measures_its_amplitude_over_root_two(self):
        audio = tone(1.0, 0.5)
        self.assertAlmostEqual(
            preset_emu.rms(audio, 0.1, 0.9), 0.5 / math.sqrt(2), places=3
        )

    def test_the_window_picks_its_part_of_the_audio(self):
        audio = tone(0.5, 0.0) + tone(0.5, 0.5)
        self.assertLess(preset_emu.rms(audio, 0.05, 0.45), gen_emu.SILENCE_RMS)
        self.assertGreater(preset_emu.rms(audio, 0.55, 0.95), preset_emu.LOUD_RMS)

    def test_a_window_past_the_audio_is_silent(self):
        self.assertEqual(preset_emu.rms(tone(0.1, 0.5), 1.0, 2.0), 0.0)


if __name__ == "__main__":
    unittest.main()
