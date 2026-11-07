"""Tests for scripts/lint/generative_rules.py. They live here so CI's and check_all.py's unittest discovery finds them.

python3 scripts/generative/test_generative_rules.py
"""

import os
import sys
import unittest

sys.path.insert(
    0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lint")
)
import generative_rules as rules

PERFORM_VIEW = "src/deluge/gui/views/perform_view.cpp"


def l10n_findings(source, path=PERFORM_VIEW):
    """(line, literal) for each perform-l10n finding in `source`."""
    text = rules.strip(source, keep_strings=True)
    return [
        (no, message.split("(", 1)[1].split("...", 1)[0])
        for no, message in rules.check_perform_l10n(path, text)
    ]


class PerformL10n(unittest.TestCase):
    def test_a_literal_passed_straight_is_caught(self):
        self.assertEqual(
            l10n_findings('display->displayPopup("HELLO");\n'), [(1, '"HELLO"')]
        )

    def test_a_literal_wrapped_onto_the_next_line_is_caught(self):
        self.assertEqual(
            l10n_findings('canvas.drawString(\n    "HELLO", 0, 0);\n'), [(2, '"HELLO"')]
        )

    def test_both_operands_of_a_ternary_are_caught(self):
        self.assertEqual(
            l10n_findings('display->displayPopup(on ? "ON" : "OFF");\n'),
            [(1, '"ON"'), (1, '"OFF"')],
        )

    def test_a_nested_ternary_and_parentheses_are_caught(self):
        self.assertEqual(
            l10n_findings('display->displayPopup((a ? "A" : b ? "B" : name), 3);\n'),
            [(1, '"A"'), (1, '"B"')],
        )

    def test_a_literal_inside_another_call_or_a_later_argument_is_not_shown_text(self):
        source = (
            'display->displayPopup(l10n::get(on ? l10n::String::A : l10n::String::B), 3, false, "x");\n'
            'canvas.drawString(lookup("KEY"), 0, 0);\n'
        )
        self.assertEqual(l10n_findings(source), [])

    def test_a_literal_behind_a_cast_is_caught(self):
        self.assertEqual(
            l10n_findings(
                'display->displayPopup((char const*)"HOLD");\n'
                'display->displayPopup(on ? (const char*)"ON" : (char const*)"OFF");\n'
            ),
            [(1, '"HOLD"'), (2, '"ON"'), (2, '"OFF"')],
        )

    def test_a_cast_literal_inside_another_call_is_not_shown_text(self):
        self.assertEqual(
            l10n_findings('display->displayPopup(lookup((char const*)"KEY"));\n'), []
        )

    def test_a_literal_with_an_encoding_prefix_is_caught(self):
        self.assertEqual(
            l10n_findings(
                'display->displayPopup(u8"HOLD");\n'
                'canvas.drawString(on ? L"ON" : u"OFF", 0, 0);\n'
            ),
            [(1, '"HOLD"'), (2, '"ON"'), (2, '"OFF"')],
        )

    def test_a_prefixed_literal_behind_a_cast_is_caught(self):
        # The only u8 form that compiles here: C++23 makes u8"" a char8_t array.
        self.assertEqual(
            l10n_findings(
                'display->displayPopup((char const*)u8"HOLD");\n'
                'display->displayPopup(on ? (wchar_t const*)L"ON" : (char16_t const*)u"OFF");\n'
                'display->displayPopup((char32_t const*)U"HOLD");\n'
            ),
            [(1, '"HOLD"'), (2, '"ON"'), (2, '"OFF"'), (3, '"HOLD"')],
        )

    def test_a_literal_behind_a_named_cast_is_caught(self):
        self.assertEqual(
            l10n_findings(
                'display->displayPopup(static_cast<char const*>("HOLD"));\n'
                'display->displayPopup(on ? reinterpret_cast<char const*>(u8"ON") : "OFF");\n'
            ),
            [(1, '"HOLD"'), (2, '"ON"'), (2, '"OFF"')],
        )

    def test_a_literal_in_a_template_call_is_not_shown_text(self):
        self.assertEqual(
            l10n_findings('display->displayPopup(lookup<char const*>("KEY"));\n'), []
        )

    def test_text_without_letters_is_allowed(self):
        self.assertEqual(l10n_findings('display->displayPopup(on ? "-" : "+");\n'), [])

    def test_the_ext_tag_is_exempt_only_on_its_own_line(self):
        exempt = 'canvas.drawStringAlignRight("EXT", kStatusY, kTextSpacingX, kTextSpacingY, right);\n'
        self.assertEqual(l10n_findings("\t\t" + exempt), [])
        self.assertEqual(
            l10n_findings('display->displayPopup("EXT");\n'), [(1, '"EXT"')]
        )
        self.assertEqual(
            l10n_findings(exempt, path="src/deluge/gui/views/other_view.cpp"),
            [(1, '"EXT"')],
        )


if __name__ == "__main__":
    unittest.main()
