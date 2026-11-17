"""Tests for covers.py: which probe scripts a change needs (#314), and that every script's covers paths exist.

python3 scripts/generative/test_covers.py
"""

import contextlib
import io
import os
import sys
import tempfile
import unittest
from typing import ClassVar

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import covers

G = "scripts/generative/"


class Repo(unittest.TestCase):
    """A scratch repo: scripts/generative/ with a few scripts, and src/ files for covers paths to name."""

    SCRIPTS: ClassVar[dict] = {
        "test_port_smoke.txt": "start_state\n",
        "perform_a.txt": "# covers: src/deluge/a.cpp src/deluge/views/\nstart_state\ninclude perform_open.txt\n",
        "perform_b.txt": "# covers: src/deluge/b_*.cpp\n# covers: src/deluge/shared.h\nstart_state\n",
        "perform_plain.txt": "# no covers line\nstart_state\n",
        "perform_open.txt": "include perform_pick.txt  # shared\n",
        "perform_pick.txt": "button 1 2 down\n",
    }
    SOURCES = (
        "src/deluge/a.cpp",
        "src/deluge/views/v.cpp",
        "src/deluge/b_one.cpp",
        "src/deluge/shared.h",
    )

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.repo = tmp.name
        self.here = os.path.join(self.repo, *G.split("/"))
        os.makedirs(self.here)
        for name, text in self.SCRIPTS.items():
            self.write(G + name, text)
        for name in self.SOURCES:
            self.write(name, "")

    def write(self, rel, text):
        path = os.path.join(self.repo, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as fh:
            fh.write(text)

    def path(self, name):
        return os.path.join(self.here, name)

    def scripts(self):
        return [
            self.path(n)
            for n in ("perform_a.txt", "perform_b.txt", "perform_plain.txt")
        ]

    def pick(self, *changed):
        return [
            os.path.basename(s)
            for s in covers.pick(self.scripts(), list(changed), self.repo)
        ]


class Pick(Repo):
    def test_a_file_a_script_covers_runs_it_and_the_scripts_with_no_covers_line(self):
        self.assertEqual(
            self.pick("src/deluge/a.cpp"), ["perform_a.txt", "perform_plain.txt"]
        )

    def test_a_file_under_a_covered_folder_or_matched_by_a_glob_runs_its_script(self):
        self.assertEqual(self.pick("src/deluge/views/deep/w.cpp")[0], "perform_a.txt")
        self.assertEqual(self.pick("src/deluge/b_two.cpp")[0], "perform_b.txt")

    def test_every_covers_line_counts(self):
        self.assertEqual(self.pick("src/deluge/shared.h")[0], "perform_b.txt")

    def test_a_folder_name_is_no_prefix_of_a_sibling(self):
        self.assertEqual(self.pick("src/deluge/views2/x.cpp"), ["perform_plain.txt"])

    def test_a_firmware_change_no_script_covers_runs_only_those_with_no_covers_line(
        self,
    ):
        self.assertEqual(self.pick("src/deluge/dsp/reverb.cpp"), ["perform_plain.txt"])

    def test_a_change_outside_the_firmware_that_no_script_covers_runs_nothing(self):
        self.assertEqual(
            self.pick(
                "docs/x.md", "scripts/generative/make_pack.py", "tests/spec/s.cpp"
            ),
            [],
        )

    def test_a_changed_script_runs_itself_even_with_no_firmware_change(self):
        self.assertEqual(
            self.pick(G + "perform_b.txt"), ["perform_b.txt", "perform_plain.txt"]
        )

    def test_a_changed_include_runs_the_scripts_that_include_it_through_others(self):
        self.assertEqual(self.pick(G + "perform_pick.txt")[0], "perform_a.txt")

    def test_a_change_to_how_every_script_runs_runs_them_all(self):
        for path in covers.EVERY_SCRIPT:
            with self.subTest(path=path):
                self.assertEqual(
                    self.pick(path),
                    ["perform_a.txt", "perform_b.txt", "perform_plain.txt"],
                )

    def test_the_firmwares_build_files_count_as_a_firmware_change(self):
        self.assertTrue(covers.firmware_changed(["CMakeLists.txt"]))
        self.assertTrue(covers.firmware_changed(["cmake/arm.cmake"]))
        self.assertFalse(
            covers.firmware_changed(["tests/CMakeLists.txt", "scripts/x.py"])
        )


class Missing(Repo):
    def test_every_covers_path_that_matches_nothing_is_listed(self):
        self.write(
            G + "perform_c.txt",
            "# covers: src/deluge/gone.cpp src/deluge/b_*.cpp src/old/\nstart_state\n",
        )
        self.assertEqual(
            covers.missing(self.scripts() + [self.path("perform_c.txt")], self.repo),
            [("perform_c.txt", "src/deluge/gone.cpp"), ("perform_c.txt", "src/old/")],
        )


class Hardware(Repo):
    def test_lists_the_scripts_a_deluge_can_run(self):
        self.write(
            G + "perform_card.txt", "# needs: card fixtures/perform_card\nstart_state\n"
        )
        self.write(G + "perform_leak.txt", "# needs: gen_emu.py leak\nstart_state\n")
        self.write(
            G + "perform_ear.txt",
            "# needs: a Deluge and someone listening\nstart_state\n",
        )
        self.write(
            G + "perform_timing.txt", "# needs: a Deluge for its timings\nstart_state\n"
        )
        self.assertEqual(
            [os.path.basename(s) for s in covers.hardware_scripts(self.here)],
            [
                "perform_a.txt",
                "perform_b.txt",
                "perform_plain.txt",
                "perform_timing.txt",
                "test_port_smoke.txt",
            ],
        )


class CheckFlag(Repo):
    def test_check_lists_each_missing_path_and_fails(self):
        self.write(G + "perform_c.txt", "# covers: src/deluge/gone.cpp\nstart_state\n")
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = covers.check(covers.all_scripts(self.here), self.repo)
        self.assertEqual(code, 1)
        self.assertIn("perform_c.txt: src/deluge/gone.cpp", out.getvalue())

    def test_check_passes_when_every_path_exists(self):
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(covers.check(covers.all_scripts(self.here), self.repo), 0)


class TheRepo(unittest.TestCase):
    """The repo's own scripts: run by the unit tests, so a renamed file can't leave a covers path behind unseen."""

    def test_every_covers_path_exists(self):
        self.assertEqual(covers.missing(covers.all_scripts()), [])

    def test_the_includes_have_no_covers_line(self):
        # An include runs as part of the scripts that include it; a covers line there would read as a script's.
        scripts = set(covers.all_scripts())
        for path in sorted(os.listdir(HERE)):
            full = os.path.join(HERE, path)
            if path.endswith(".txt") and full not in scripts:
                with open(full) as fh:
                    self.assertIsNone(covers.covers(fh.read()), path)


if __name__ == "__main__":
    unittest.main()
