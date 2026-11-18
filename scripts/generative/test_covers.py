"""Tests for covers.py: which probe scripts a change needs (#314), and that every script's covers paths exist.

python3 scripts/generative/test_covers.py
"""

import contextlib
import io
import os
import subprocess
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
        "test_port_emu.py": "import deluge_probe\nimport fat_image, os\n",
        "deluge_probe.py": "import os\n    import hardware_session\n",
        "fat_image.py": "import struct\n",
        "hardware_session.py": "from time import sleep\n",
        "gen_emu.py": "import test_port_emu\nimport wav_levels\n",
        "wav_levels.py": "",
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

    def test_the_runner_files_are_the_runners_imports_followed_through(self):
        self.assertEqual(
            covers.module_files("test_port_emu.py", here=self.here, repo=self.repo),
            {
                G + n
                for n in (
                    "test_port_emu.py",
                    "deluge_probe.py",
                    "fat_image.py",
                    "hardware_session.py",
                )
            },
        )

    def test_a_change_to_how_every_script_runs_runs_them_all(self):
        for path in sorted(covers.runner_files(self.here, self.repo)):
            with self.subTest(path=path):
                self.assertEqual(
                    self.pick(path),
                    ["perform_a.txt", "perform_b.txt", "perform_plain.txt"],
                )

    def test_the_firmwares_build_files_count_as_a_firmware_change(self):
        for path in (
            "CMakeLists.txt",
            "lib/CMakeLists.txt",
            "linker_script_rz_a1l.ld",
            "toolchain/REQUIRED_VERSION",
            "scripts/toolchain/dbtenv.sh",
            "scripts/tasks/task-build.py",
            "scripts/cmake/CMakeToolchainDeluge.cmake",  # the root CMakeLists.txt's CMAKE_TOOLCHAIN_FILE
            "dbt",
            G + "wav_levels.py",  # gen_emu.py's: smoke and the leak check run on it
            G + "step_section.py",  # reads the Release build
        ):
            with self.subTest(path=path):
                self.assertTrue(covers.firmware_changed([path], self.repo))
        self.assertFalse(
            covers.firmware_changed(
                ["tests/CMakeLists.txt", "scripts/x.py", "scripts/tasks/task-test.py"],
                self.repo,
            )
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

    def test_a_glob_counts_as_matching_what_pick_matches(self):
        # pick() matches with fnmatch, where * crosses /: missing() must agree, or a live path reads as gone.
        self.write(
            G + "perform_c.txt", "# covers: src/*.cpp src/deluge/*/v.cpp\nstart_state\n"
        )
        self.assertEqual(covers.missing([self.path("perform_c.txt")], self.repo), [])


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
    def setUp(self):
        super().setUp()
        subprocess.run(["git", "init", "-q"], cwd=self.repo, check=True)
        subprocess.run(["git", "add", "-A"], cwd=self.repo, check=True)

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


class Events(Repo):
    """Each src/ file that sends an event a script expects must be on its covers lines (or every script's)."""

    def setUp(self):
        super().setUp()
        self.write("src/deluge/a.cpp", 'TEST_EVENT("gen_alpha part=%d", p);\n')
        self.write(
            "src/deluge/other.cpp",
            'out.add("gen_alpha stopped");\nTEST_EVENT("gen_beta");\n',
        )
        self.write(
            G + "perform_pick.txt", "button 1 2 down\nexpect event /^gen_beta$/\n"
        )
        self.write(
            G + "perform_a.txt",
            "# covers: src/deluge/a.cpp\nstart_state\ninclude perform_open.txt\n"
            "expect event gen_alpha part=1\nexpect no_event gen_gamma within=1\nkeep events gen_delta\n",
        )
        subprocess.run(["git", "init", "-q"], cwd=self.repo, check=True)
        subprocess.run(["git", "add", "-A"], cwd=self.repo, check=True)

    def test_the_events_a_script_and_its_includes_expect(self):
        sent = covers.emitters(self.repo)
        self.assertEqual(
            covers.expected_events(self.path("perform_a.txt"), sent),
            {"gen_alpha", "gen_beta"},
        )

    def test_a_sender_the_covers_lines_miss_is_listed(self):
        self.assertEqual(
            covers.uncovered([self.path("perform_a.txt")], self.repo),
            [
                ("perform_a.txt", "gen_alpha", "src/deluge/other.cpp"),
                ("perform_a.txt", "gen_beta", "src/deluge/other.cpp"),
            ],
        )

    def test_a_kind_alone_matches_every_kind_it_begins(self):
        sent = {"gen_h1": {"a"}, "gen_h16": {"b"}, "gen_h2": {"c"}}
        self.assertEqual(covers.expect_kinds("gen_h1", sent), {"gen_h1", "gen_h16"})
        self.assertEqual(covers.expect_kinds("gen_h1 steps=", sent), {"gen_h1"})
        self.assertEqual(covers.expect_kinds("/^gen_h1 steps=\\d+/", sent), {"gen_h1"})
        # a regex whose kind no space follows matches longer kinds too, anchored or not
        for text in ("/gen_h1/", "/^gen_h1/", "/^gen_h1\\d*/"):
            with self.subTest(text=text):
                self.assertEqual(covers.expect_kinds(text, sent), {"gen_h1", "gen_h16"})

    def test_an_expect_that_starts_with_no_sent_kind_has_none(self):
        sent = {"gen_sidebar": {"a"}, "gen_arcpage": {"b"}}
        for text in (
            "force=B",
            "EXT",
            "|NEXT BREAK",
            ".2",
            "/force=B/",
            "gen_nothing x=1",
        ):
            with self.subTest(text=text):
                self.assertIsNone(covers.expect_kinds(text, sent))
        self.assertEqual(
            covers.expect_kinds("/^gen_sidebar .*force=B/", sent), {"gen_sidebar"}
        )

    def test_kindless_names_each_such_expect_with_where_it_is(self):
        self.write(
            G + "perform_b.txt",
            "# covers: src/deluge/b_*.cpp\nstart_state\nexpect event force=B within=2\n",
        )
        subprocess.run(["git", "add", "-A"], cwd=self.repo, check=True)
        self.assertEqual(
            covers.kindless(
                [self.path("perform_a.txt"), self.path("perform_b.txt")], self.repo
            ),
            # gen_gamma and gen_delta: kinds no file sends
            [
                ("perform_a.txt:5", "gen_gamma"),
                ("perform_a.txt:6", "gen_delta"),
                ("perform_b.txt:3", "force=B"),
            ],
        )

    def test_a_failed_git_grep_is_an_error_not_no_senders(self):
        with (
            tempfile.TemporaryDirectory() as not_a_repo,
            self.assertRaises(RuntimeError),
        ):
            covers.emitters(not_a_repo)

    def test_a_file_that_builds_an_events_text_counts_as_its_sender(self):
        self.write("src/deluge/build_alpha.cpp", "")
        builders = {"gen_alpha": ("src/deluge/build_alpha.*",)}
        missed = covers.uncovered([self.path("perform_a.txt")], self.repo, builders)
        self.assertIn(
            ("perform_a.txt", "gen_alpha", "src/deluge/build_alpha.cpp"), missed
        )

    def test_a_script_with_no_covers_line_has_none_to_miss(self):
        self.assertEqual(
            covers.uncovered([self.path("perform_plain.txt")], self.repo), []
        )


class BuilderGaps(Repo):
    """builder_gaps(): a %s sender kind with no BUILDERS entry, and a TEST_EVENT("%s") file no RELAYS entry names."""

    def setUp(self):
        super().setUp()
        self.write(
            "src/deluge/x.cpp",
            'TEST_EVENT("gen_alpha %s", t);\nTEST_EVENT("gen_beta n=%d", n);\n',
        )
        self.write("src/deluge/relay.cpp", 'TEST_EVENT("%s", text);\n')
        subprocess.run(
            ["git", "init", "-q"], cwd=self.repo, check=True
        )  # untracked files count (--untracked)

    def test_lists_each_kind_and_relay_missing(self):
        self.assertEqual(
            covers.builder_gaps(self.repo, builders={}, relays={}),
            [
                "gen_alpha: TEST_EVENT fills it with %s and covers.BUILDERS has no entry",
                "src/deluge/relay.cpp: sends text built elsewhere and covers.RELAYS doesn't name it",
            ],
        )

    def test_entries_close_them(self):
        self.assertEqual(
            covers.builder_gaps(
                self.repo,
                builders={"gen_alpha": ()},
                relays={"b.h": ("src/deluge/relay.*",)},
            ),
            [],
        )


class ExpectLine(unittest.TestCase):
    def test_the_options_come_off_in_either_order(self):
        for line in (
            "expect no_event gen_x a=1 since=mark within=1",
            "expect no_event gen_x a=1 within=1 since=mark",
        ):
            with self.subTest(line=line):
                self.assertEqual(covers.EXPECT_LINE.match(line).group(1), "gen_x a=1")


class ChangedPaths(Repo):
    def test_a_failed_git_reads_as_unknown_so_everything_runs(self):
        # not a git repo: git diff fails, which must not read as "nothing changed"
        self.assertIsNone(covers.changed_paths("HEAD", self.repo))

    def test_a_moved_file_lists_its_old_path_too(self):
        def git(*args):
            subprocess.run(
                ["git", *args], cwd=self.repo, check=True, capture_output=True
            )

        git("init", "-q")
        git("add", "-A")
        git(
            "-c",
            "user.name=t",
            "-c",
            "user.email=t@t",
            "-c",
            "commit.gpgsign=false",
            "commit",
            "-qm",
            "base",
        )
        git("mv", "src/deluge/a.cpp", "src/deluge/moved.cpp")
        changed = covers.changed_paths("HEAD", self.repo)
        self.assertIn("src/deluge/a.cpp", changed)
        self.assertIn("src/deluge/moved.cpp", changed)


class TheRepo(unittest.TestCase):
    """The repo's own scripts: run by the unit tests, so a renamed file can't leave a covers path behind unseen."""

    def test_every_covers_path_exists(self):
        self.assertEqual(covers.missing(covers.all_scripts()), [])

    def test_every_sender_of_an_expected_event_is_covered(self):
        self.assertEqual(covers.uncovered(covers.all_scripts()), [])

    def test_every_event_whose_text_another_file_fills_has_a_builders_entry(self):
        # covers.check() runs builder_gaps() on every check_all run; here as a unit test too
        self.assertEqual(covers.builder_gaps(), [])

    def test_an_l10n_name_change_runs_the_scripts_that_read_control_names(self):
        # gen_edit's control=BRI and gen_controls ranges' BRI= come from l10n through upperControlName()
        picked = {
            os.path.basename(s)
            for s in covers.pick(
                covers.probe_scripts()[0], ["src/deluge/gui/l10n/english.json"]
            )
        }
        self.assertTrue(
            {"perform_homes.txt", "perform_sound_controls.txt", "perform_retire.txt"}
            <= picked,
            picked,
        )

    def test_a_script_missing_a_builder_of_an_event_it_expects_fails(self):
        # perform_menu.txt expects /^gen_status .*\|>KEY/, whose ">KEY" queueTag() in queue_text.cpp builds
        with open(os.path.join(HERE, "perform_menu.txt")) as fh:
            text = fh.read()
        self.assertIn("src/deluge/generative/perform/queue_text.*", text)
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "perform_menu.txt")
            with open(path, "w") as fh:
                fh.write(text.replace("src/deluge/generative/perform/queue_text.*", ""))
            missed = covers.uncovered([path])
        self.assertIn(
            (
                "perform_menu.txt",
                "gen_status",
                "src/deluge/generative/perform/queue_text.cpp",
            ),
            missed,
        )

    def test_every_expect_starts_with_its_kind(self):
        self.assertEqual(covers.kindless(covers.all_scripts()), [])

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
