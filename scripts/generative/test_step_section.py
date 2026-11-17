"""Tests for step_section.py: the step's call targets that leave .text.generative_step, by address.

python3 scripts/generative/test_step_section.py
"""

import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import step_section

MAP = """\
 .text          0x20100000     0x1000 a.o
 .ARM.extab.text.generative_step
                0x20000400       0x10 b.lto.o
 .text.generative_step
                0x20002000      0x100 /tmp/cc.lto.o
 .text.unlikely
                0x20002100       0x84 /tmp/cc.lto.o
"""

# objdump -d (no -C): addresses and raw bytes, then a tab, the mnemonic, a tab and the operands.
DISASSEMBLY = """\
20002000 <_ZN6deluge10generative9Generator14decideNextStepEv>:
20002000:\tb510      \tpush\t{r4, lr}
20002002:\tf000 f805 \tbl\t20002010 <_ZN6deluge10generative3Arc8headroomEllb>
20002006:\tf7ff fffb \tbl\t20001000 <__divsi3>
2000200a:\tb1bd      \tcbz\tr5, 20002030 <_ZN6deluge10generative9Generator14decideNextStepEv+0x30>
2000200c:\tf000 b800 \tb.w\t20003000 <_ZN6deluge10generative9Generator9endAtStopEv>
20002010:\tf000 f800 \tbl\t20003040 <_ZNSt3__16__sortIN6deluge10generative5EventEEEvPT_>
20002014:\t4798      \tblx\tr3
20002016:\td1f3      \tbne.n\t20003080 <_ZN6deluge10generative6detail4walkIJLi3EEEEvv+0x8>
20002018:\tf3af 8000 \tbfi\tr0, r1, #0, #4
2000201a:\t4770      \tbx\tlr
"""

# nm -n -C: address, type, demangled name (with spaces, and template '>' in it).
SYMBOLS = """\
20001000 T __divsi3
20002000 t deluge::generative::Generator::decideNextStep()
20002010 t deluge::generative::Arc::headroom(long, long, bool) const
20003000 t deluge::generative::Generator::endAtStop()
20003040 W void std::__sort<deluge::generative::Event>(deluge::generative::Event*)
20003078 t long deluge::generative::detail::walk<(operator>(3, 2))>()
200030a0 d some_data
"""


class StepSection(unittest.TestCase):
    def test_the_range_is_the_sections_line_in_the_map(self):
        self.assertEqual(step_section.section_range(MAP), (0x20002000, 0x20002100))

    def test_a_map_without_the_section_is_an_error(self):
        with self.assertRaisesRegex(ValueError, "generative_step"):
            step_section.section_range(" .text 0x1 0x2 a.o\n")

    def test_a_second_contributor_to_the_section_is_an_error(self):
        # The check reads one range: a second object's .text.generative_step would go unchecked.
        second = (
            MAP
            + " .text.generative_step\n                0x20004000       0x40 /tmp/cc2.lto.o\n"
        )
        with self.assertRaises(ValueError):
            step_section.section_range(second)

    def test_size_0_entries_hold_no_code_and_are_skipped(self):
        discarded = " .text.generative_step\n                0x20004000        0x0 /tmp/cc2.lto.o\n"
        self.assertEqual(
            step_section.section_range(discarded + MAP + discarded),
            (0x20002000, 0x20002100),
        )

    def test_branch_targets_are_read_by_address_whatever_the_name(self):
        self.assertEqual(
            step_section.branch_targets(DISASSEMBLY),
            [0x20002010, 0x20001000, 0x20002030, 0x20003000, 0x20003040, 0x20003080],
        )

    def test_a_target_is_named_by_the_function_it_lies_in(self):
        symbols = step_section.text_symbols(SYMBOLS)
        report = step_section.check((0, 0), [0x20003080, 0x20000000], symbols)
        self.assertIn(
            "long deluge::generative::detail::walk<(operator>(3, 2))>()",
            report.generative_outside,
        )
        self.assertIn("0x20000000", report.other_outside)

    def test_generative_means_the_function_is_in_the_namespace_not_its_arguments(self):
        self.assertTrue(
            step_section.in_generative("deluge::generative::Arc::headroom(long) const")
        )
        self.assertTrue(
            step_section.in_generative(
                "long deluge::generative::detail::walk<(operator>(3, 2))>()"
            )
        )
        self.assertFalse(
            step_section.in_generative(
                "void std::__sort<deluge::generative::Event>(deluge::generative::Event*)"
            )
        )
        self.assertFalse(step_section.in_generative("__divsi3"))

    def test_lists_the_generative_targets_outside_and_the_others_apart(self):
        report = step_section.check(
            (0x20002000, 0x20002100),
            step_section.branch_targets(DISASSEMBLY),
            step_section.text_symbols(SYMBOLS),
        )
        self.assertEqual(
            report.generative_outside,
            {
                "deluge::generative::Generator::endAtStop()": 0x20003000,
                "long deluge::generative::detail::walk<(operator>(3, 2))>()": 0x20003078,
            },
        )
        self.assertEqual(
            sorted(report.other_outside),
            [
                "__divsi3",
                "void std::__sort<deluge::generative::Event>(deluge::generative::Event*)",
            ],
        )

    def test_a_clean_section_reports_no_generative_target(self):
        inside = "20002002:\tf000 f805 \tbl\t20002010 <x>\n"
        report = step_section.check(
            (0x20002000, 0x20002100),
            step_section.branch_targets(inside),
            step_section.text_symbols(SYMBOLS),
        )
        self.assertEqual(report.generative_outside, {})


if __name__ == "__main__":
    unittest.main()
