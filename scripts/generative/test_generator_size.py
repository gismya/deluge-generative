"""Tests for generator_size.py: sizeof(Generator) on ARM against H18 and spec §20.

python3 scripts/generative/test_generator_size.py
"""

import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import generator_size

ERRORS = """\
<stdin>:4:46: error: aggregate 'Size<13304> generatorSize' has incomplete type and cannot be defined
<stdin>:5:45: error: aggregate 'Size<172> partSpecSize' has incomplete type and cannot be defined
"""


class GeneratorSize(unittest.TestCase):
    def test_reads_each_size_from_the_compilers_errors(self):
        self.assertEqual(
            generator_size.sizes_from(ERRORS),
            {"generatorSize": 13304, "partSpecSize": 172},
        )

    def test_reads_the_limit_from_the_static_assert(self):
        header = 'static_assert(sizeof(Generator) <= 16 * 1024, "the Generator state block outgrew");\n'
        self.assertEqual(generator_size.limit_from(header), 16384)

    def test_reads_the_figure_spec_20_states(self):
        text = "The POC's core was 1664 B.\n\nThe core is 13,304 B of state per Generator block on ARM (...)\n"
        self.assertEqual(generator_size.spec_figure(text), 13304)

    def test_passes_when_it_fits_and_matches_spec_20(self):
        ok, lines = generator_size.verdict(13304, limit=16384, spec=13304)
        self.assertTrue(ok)
        self.assertIn("3,080 B left", lines[0])

    def test_fails_when_spec_20_states_another_size(self):
        ok, lines = generator_size.verdict(13312, limit=16384, spec=13304)
        self.assertFalse(ok)
        self.assertIn("update spec §20", lines[-1])

    def test_fails_over_the_limit(self):
        ok, lines = generator_size.verdict(16388, limit=16384, spec=16388)
        self.assertFalse(ok)
        self.assertIn("over H18", lines[-1])

    def test_the_repo_states_a_figure_and_a_limit(self):
        self.assertGreater(
            generator_size.spec_figure(generator_size.read(generator_size.SPEC)), 0
        )
        self.assertGreater(
            generator_size.limit_from(generator_size.read(generator_size.HEADER)), 0
        )


if __name__ == "__main__":
    unittest.main()
