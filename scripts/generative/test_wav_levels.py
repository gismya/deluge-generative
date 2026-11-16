"""Tests for wav_levels.py: the levels of WAV captures, with no numpy.

python3 scripts/generative/test_wav_levels.py
"""

import math
import os
import struct
import sys
import tempfile
import unittest
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import wav_levels

RATE = 1000


def write(path, frames, width=4, channels=2, rate=RATE):
    """frames: a list of per-channel tuples of floats, full scale 1.0."""
    full = 2 ** (8 * width - 1) - 1
    fmt = {2: "h", 4: "i"}.get(width)
    data = bytearray()
    for frame in frames:
        for v in frame[:channels]:
            n = max(-full - 1, min(full, round(v * full)))
            if width == 3:
                data += n.to_bytes(3, "little", signed=True)
            else:
                data += struct.pack("<" + fmt, n)
    with wave.open(path, "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(width)
        w.setframerate(rate)
        w.writeframes(bytes(data))


def sine(amplitude, seconds=1.0, hz=50):
    return [
        (amplitude * math.sin(2 * math.pi * hz * i / RATE),) * 2
        for i in range(int(seconds * RATE))
    ]


class Levels(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.dir.cleanup()

    def path(self, name):
        return os.path.join(self.dir.name, name)

    def test_a_half_scale_sine_is_9_dB_rms_and_6_dB_peak_below_full_scale(self):
        write(self.path("a.wav"), sine(0.5))
        level = wav_levels.measure(self.path("a.wav"))
        self.assertAlmostEqual(level.rms_db, -9.03, places=1)
        self.assertAlmostEqual(level.peak_db, -6.02, places=1)
        self.assertEqual(level.full_scale, 0)

    def test_counts_samples_at_full_scale_on_either_side(self):
        write(self.path("a.wav"), [(1.0, -1.0), (0.0, 0.0), (1.0, 0.2)])
        self.assertEqual(wav_levels.measure(self.path("a.wav")).full_scale, 3)

    def test_reads_16_and_24_bit_and_mono(self):
        for width, channels in ((2, 2), (3, 2), (4, 1)):
            write(self.path("a.wav"), sine(0.5), width=width, channels=channels)
            level = wav_levels.measure(self.path("a.wav"))
            self.assertAlmostEqual(level.rms_db, -9.03, places=1, msg=(width, channels))

    def test_skip_leaves_out_the_first_seconds(self):
        write(self.path("a.wav"), sine(1.0, seconds=1.0) + sine(0.1, seconds=1.0))
        level = wav_levels.measure(self.path("a.wav"), skip=1.0)
        self.assertAlmostEqual(level.peak_db, -20.0, places=1)

    def test_silence_reads_as_minus_infinity(self):
        write(self.path("a.wav"), [(0.0, 0.0)] * 10)
        level = wav_levels.measure(self.path("a.wav"))
        self.assertEqual(level.rms_db, -math.inf)
        self.assertIn("-inf", wav_levels.describe("a.wav", level))

    def test_a_part_gated_out_is_the_energy_the_capture_without_it_lacks(self):
        # The full mix is two equal uncorrelated halves, so the part is 3 dB under the full mix, 0 dB against the rest.
        full = wav_levels.Level(mean_square=0.02, peak=0.5, full_scale=0, frames=1)
        without = wav_levels.Level(mean_square=0.01, peak=0.4, full_scale=0, frames=1)
        part_db, versus_rest = wav_levels.missing_part(full, without)
        self.assertAlmostEqual(part_db, 10 * math.log10(0.01), places=6)
        self.assertAlmostEqual(versus_rest, 0.0, places=6)

    def test_a_capture_louder_than_the_full_mix_has_no_estimate(self):
        full = wav_levels.Level(mean_square=0.01, peak=0.5, full_scale=0, frames=1)
        louder = wav_levels.Level(mean_square=0.02, peak=0.5, full_scale=0, frames=1)
        self.assertIsNone(wav_levels.missing_part(full, louder))

    def test_main_prints_a_line_per_file_and_the_missing_part_against_the_full_mix(
        self,
    ):
        write(self.path("full.wav"), sine(0.5))
        write(self.path("without.wav"), sine(0.25))
        lines = []
        code = wav_levels.main(
            ["--full", self.path("full.wav"), self.path("without.wav")],
            out=lines.append,
        )
        self.assertEqual(code, 0)
        self.assertEqual(len(lines), 2)
        self.assertIn("full.wav", lines[0])
        self.assertIn("without.wav", lines[1])
        self.assertIn("missing part", lines[1])

    def test_a_file_that_isnt_pcm_wav_is_an_error(self):
        with open(self.path("a.wav"), "wb") as fh:
            fh.write(b"not a wav")
        with self.assertRaises(SystemExit):
            wav_levels.main([self.path("a.wav")], out=lambda line: None)


if __name__ == "__main__":
    unittest.main()
