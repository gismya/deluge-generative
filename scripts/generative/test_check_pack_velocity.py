"""Tests for check_pack_velocity.py, run through its command line on throwaway pack folders.

python3 scripts/generative/test_check_pack_velocity.py
"""

import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "check_pack_velocity.py")

HEADER = '<?xml version="1.0" encoding="UTF-8"?>\n<firmwareVersion>c1.3.0</firmwareVersion>\n'


def cable(source, amount="0x20000000"):
    return f"<patchCable><source>{source}</source><destination>volume</destination><amount>{amount}</amount></patchCable>"


def tag_preset(*cables):
    return (
        HEADER
        + f"<sound><defaultParams><patchCables>{''.join(cables)}</patchCables></defaultParams></sound>\n"
    )


def attribute_preset(source):
    return (
        HEADER
        + f'<sound><defaultParams><patchCables><patchCable source="{source}" destination="volume" '
        'amount="0x20000000" /></patchCables></defaultParams></sound>\n'
    )


def dx_preset(velocity_sensitivity):
    # 156 bytes: six operators of 21 (key velocity sensitivity is each operator's byte 15), then the voice's own.
    voice = bytearray(156)
    voice[5 * 21 + 15] = velocity_sensitivity
    return (
        HEADER
        + f"<sound><osc1><type>dx7</type><dx7patch>{voice.hex().upper()}</dx7patch></osc1></sound>\n"
    )


class CheckPackVelocity(unittest.TestCase):
    def run_on(self, presets):
        with tempfile.TemporaryDirectory() as pack:
            for path, text in presets.items():
                full = os.path.join(pack, path)
                os.makedirs(os.path.dirname(full), exist_ok=True)
                with open(full, "w") as f:
                    f.write(text)
            return subprocess.run(
                [sys.executable, SCRIPT, pack],
                capture_output=True,
                text=True,
                check=False,
            )

    def test_passes_presets_with_a_velocity_cable_written_either_way(self):
        result = self.run_on(
            {
                "SYNTHS/GENERATIVE/FX/A.XML": tag_preset(
                    cable("envelope2"), cable("velocity")
                ),
                "SYNTHS/GENERATIVE/B.XML": attribute_preset("velocity"),
            }
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("2 presets", result.stdout)

    def test_fails_naming_each_preset_without_one(self):
        result = self.run_on(
            {
                "SYNTHS/GENERATIVE/FX/A.XML": tag_preset(cable("envelope2")),
                "SYNTHS/GENERATIVE/B.XML": tag_preset(
                    cable("velocity", amount="0x00000000")
                ),
                "SYNTHS/GENERATIVE/C.XML": tag_preset(cable("velocity")),
            }
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("A.XML", result.stdout)
        self.assertIn("B.XML", result.stdout)
        self.assertNotIn("C.XML", result.stdout)

    def test_counts_dx7_operator_velocity_sensitivity(self):
        self.assertEqual(
            self.run_on({"SYNTHS/GENERATIVE/DX/A.XML": dx_preset(3)}).returncode, 0
        )
        self.assertEqual(
            self.run_on({"SYNTHS/GENERATIVE/DX/A.XML": dx_preset(0)}).returncode, 1
        )

    def test_passes_the_repo_pack(self):
        result = subprocess.run(
            [sys.executable, SCRIPT], capture_output=True, text=True, check=False
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
