"""The probe scripts' Recipe fixtures follow a retune (#488): a file meant to be today's names its genre's current
version, so a retune that bumps the genre bumps the fixtures and the scripts' expects with it (spec §21.2, A retune).

python3 scripts/generative/test_recipe_fixtures.py
"""

import glob
import os
import re
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
RECIPES = os.path.join(HERE, "fixtures", "recipe_load", "RECIPES")
GOLDENS = os.path.join(ROOT, "tests", "spec", "generator", "goldens")
# Files written by an older version on purpose: they check the version notice.
OLDER = {"LOAD OLDER"}


def golden_version(genre):
    """The genre's current version, off its golden's header, or None for a genre with no golden."""
    path = os.path.join(GOLDENS, genre + ".txt")
    if not os.path.exists(path):
        return None
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = re.match(r"genre (\S+) version (\d+)$", line.strip())
            if m:
                return int(m.group(2))
    raise AssertionError(f"{path} has no version line")


def fixtures():
    """{name: (genre, version)} for each Recipe fixture."""
    out = {}
    for path in sorted(glob.glob(os.path.join(RECIPES, "*.XML"))):
        with open(path, encoding="utf-8") as f:
            text = f.read()
        genre = re.search(r'\sgenre="([^"]+)"', text).group(1)
        version = int(re.search(r'\sgenreVersion="(\d+)"', text).group(1))
        out[os.path.splitext(os.path.basename(path))[0]] = (genre, version)
    return out


def scripts():
    """(script, line number, line) for every probe script line."""
    for path in sorted(glob.glob(os.path.join(HERE, "*.txt"))):
        with open(path, encoding="utf-8") as f:
            for number, line in enumerate(f, 1):
                yield os.path.basename(path), number, line


class RecipeFixtureVersions(unittest.TestCase):
    def test_current_fixtures_name_the_current_version(self):
        found = fixtures()
        self.assertTrue(found)
        for name, (genre, version) in found.items():
            current = golden_version(genre)
            if current is None:
                continue
            with self.subTest(name=name):
                if name in OLDER:
                    self.assertLess(version, current)
                else:
                    self.assertEqual(
                        version,
                        current,
                        f'{name}.XML: genreVersion="{version}", but {genre} is version {current}: '
                        "a retune bumps the fixtures it reaches",
                    )

    def test_scripts_expect_the_fixtures_versions(self):
        found = fixtures()
        chose = re.compile(
            r"gen_recipe_load chose name=(.+?) genre=(\S+) version=(\d+)"
        )
        older = re.compile(r"gen_recipe_load version older file=(\d+) genre=(\d+)")
        for script, number, line in scripts():
            m = chose.search(line)
            if m and m.group(1) in found:
                with self.subTest(script=script, line=number):
                    self.assertEqual((m.group(2), int(m.group(3))), found[m.group(1)])
            m = older.search(line)
            if m:
                with self.subTest(script=script, line=number):
                    # Every version notice in the scripts is LOAD OLDER's, an Acid techno file.
                    self.assertEqual(int(m.group(1)), found["LOAD OLDER"][1])
                    self.assertEqual(int(m.group(2)), golden_version("techno-acid"))


if __name__ == "__main__":
    unittest.main()
