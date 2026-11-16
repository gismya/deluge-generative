"""Check that every synth preset in the Generative pack responds to velocity (spec §10.1).

A preset passes if it has a patch cable from velocity with a non-zero amount, or (for a DX7 voice) an operator with
key velocity sensitivity above 0. The Generator never adds cables, so a preset that fails plays every note at one
level. Run locally after adding or changing a pack preset:

    python3 scripts/generative/check_pack_velocity.py [PACK_DIR]   # default: the repo's pack/

Exits 1, naming each failing preset, if any fails. Stdlib only.
"""

import argparse
import os
import re
import sys
import xml.etree.ElementTree as ET

DEFAULT_PACK = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "..", "pack"
)
# A DX7 voice as the firmware stores it: six operators of 21 bytes, key velocity sensitivity at byte 15 of each.
DX_OPERATOR_BYTES = 21
DX_VELOCITY_SENSITIVITY = 15


def field(element, name):
    """A field the firmware may write as a child tag or as an attribute."""
    if name in element.attrib:
        return element.attrib[name].strip()
    child = element.find(name)
    return child.text.strip() if child is not None and child.text else ""


def parse(path):
    """The preset's elements. Deluge files have several top-level tags, so they are wrapped in one root."""
    with open(path, encoding="utf-8") as f:
        text = re.sub(r"^\s*<\?xml[^>]*\?>", "", f.read())
    return ET.fromstring("<file>" + text + "</file>")


def has_velocity_cable(root):
    for patch_cable in root.iter("patchCable"):
        if (
            field(patch_cable, "source") == "velocity"
            and int(field(patch_cable, "amount") or "0", 0) != 0
        ):
            return True
    return False


def has_dx_velocity(root):
    for element in root.iter():
        patch = element.attrib.get("dx7patch") or (
            element.text if element.tag == "dx7patch" else None
        )
        if not patch:
            continue
        voice = bytes.fromhex(patch.strip())
        for operator in range(6):
            index = operator * DX_OPERATOR_BYTES + DX_VELOCITY_SENSITIVITY
            if index < len(voice) and voice[index] > 0:
                return True
    return False


def presets(pack):
    synths = os.path.join(pack, "SYNTHS")
    for folder, _, files in os.walk(synths):
        for name in sorted(files):
            if name.upper().endswith(".XML") and not name.startswith("."):
                yield os.path.join(folder, name)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "pack",
        nargs="?",
        default=DEFAULT_PACK,
        help="the pack folder, laid out as on the card",
    )
    args = parser.parse_args()
    checked = 0
    failed = []
    for path in sorted(presets(args.pack)):
        checked += 1
        root = parse(path)
        if not (has_velocity_cable(root) or has_dx_velocity(root)):
            failed.append(os.path.relpath(path, args.pack))
    for path in failed:
        print("no velocity response: " + path)
    print(f"checked {checked} presets, {len(failed)} without velocity response")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
