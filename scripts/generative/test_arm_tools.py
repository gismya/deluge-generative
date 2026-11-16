"""Tests for arm_tools.py's nm reader, which step_section.py, decode_pad_crash.py and ./dbt sizediff share.

python3 scripts/generative/test_arm_tools.py
"""

import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import arm_tools
from arm_tools import Symbol

# nm -n -S -C: a size column on some lines only, names with spaces, a Thumb function at an odd address, data whose
# one-letter type is also a hex digit, and an undefined symbol with no address.
NM = """\
         U memcpy
20001001 00000010 T __divsi3
20002000 t deluge::generative::Generator::decideNextStep()
20002010 00000008 W void std::__sort<deluge::generative::Event>(deluge::generative::Event*)
20002021 00000004 d odd_byte
20003000 b c
"""


class ParseNm(unittest.TestCase):
    def test_reads_every_defined_symbol_with_its_size_when_given(self):
        self.assertEqual(
            arm_tools.parse_nm(NM),
            [
                Symbol(0x20001000, 0x10, "T", "__divsi3"),
                Symbol(
                    0x20002000,
                    None,
                    "t",
                    "deluge::generative::Generator::decideNextStep()",
                ),
                Symbol(
                    0x20002010,
                    8,
                    "W",
                    "void std::__sort<deluge::generative::Event>(deluge::generative::Event*)",
                ),
                Symbol(0x20002021, 4, "d", "odd_byte"),
                Symbol(0x20003000, None, "b", "c"),
            ],
        )


class CodeSymbols(unittest.TestCase):
    def test_finds_the_code_symbol_an_address_lies_in_skipping_data(self):
        code = arm_tools.CodeSymbols(arm_tools.parse_nm(NM))
        self.assertEqual(code.at(0x20001004).name, "__divsi3")
        self.assertEqual(
            code.at(0x20002030).name,
            "void std::__sort<deluge::generative::Event>(deluge::generative::Event*)",
        )
        self.assertEqual(code.at(0x20003004).address, 0x20002010)
        self.assertIsNone(code.at(0x20000FFF))


if __name__ == "__main__":
    unittest.main()
