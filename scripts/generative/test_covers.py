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
from unittest import mock

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
            G + "capture_pull.txt",
            "# needs: a Deluge and someone to pull its power\nstart_state\n",
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


class FullRule(Repo):
    """Whether a change needs check_all.py --full before its merge, or its covers pick suffices (#417 slice 3)."""

    def rule(self, *changed, into_generative=False):
        return covers.full_rule(
            list(changed), self.scripts(), into_generative, self.repo
        )

    def test_src_files_a_covers_line_names_need_only_the_pick(self):
        self.assertIsNone(
            self.rule(
                "src/deluge/a.cpp",
                "src/deluge/views/deep/w.cpp",
                "src/deluge/b_two.cpp",
            )
        )

    def test_a_change_outside_src_and_the_runner_needs_only_the_pick(self):
        self.assertIsNone(self.rule("docs/x.md", G + "perform_a.txt", G + "listen.py"))

    def test_a_src_file_no_covers_line_names_needs_full(self):
        self.assertEqual(
            self.rule("src/deluge/a.cpp", "src/deluge/dsp/reverb.cpp"),
            "src/deluge/dsp/reverb.cpp is on no probe script's covers lines",
        )

    def test_a_firmware_input_outside_src_no_covers_line_names_needs_full(self):
        # pick() runs only smoke and the scripts with no covers line for these, as for an uncovered src/ file
        for path in (
            "lib/fatfs/ff.c",
            "CMakeLists.txt",
            "linker_script_rz_a1l.ld",
            "toolchain/v20/x.cmake",
            "scripts/cmake/arm.cmake",
        ):
            with self.subTest(path):
                self.assertEqual(
                    self.rule("docs/x.md", path),
                    f"{path} is on no probe script's covers lines",
                )

    def test_a_firmware_input_outside_src_a_covers_line_names_needs_only_the_pick(self):
        self.write(G + "perform_lib.txt", "# covers: lib/fatfs/\nstart_state\n")
        rule = covers.full_rule(
            ["lib/fatfs/ff.c"],
            [*self.scripts(), self.path("perform_lib.txt")],
            False,
            self.repo,
        )
        self.assertIsNone(rule)

    def test_the_emulator_checks_code_and_what_picks_them_need_full(self):
        self.write(G + "check_all.py", "import covers\n")
        self.write(G + "covers.py", "")
        self.write(G + "delugemu.pin", "")
        for name in (
            "test_port_emu.py",
            "deluge_probe.py",
            "fat_image.py",  # test_port_emu.py imports it
            "gen_emu.py",
            "wav_levels.py",  # gen_emu.py imports it
            "check_all.py",
            "covers.py",
            "delugemu.pin",
        ):
            with self.subTest(name):
                self.assertEqual(
                    self.rule(G + name),
                    f"{G}{name} changed: the emulator checks run on it or it picks them",
                )

    def test_a_merge_into_generative_always_needs_full(self):
        self.assertEqual(
            self.rule("docs/x.md", into_generative=True), "a merge into generative"
        )

    def test_no_diff_needs_full(self):
        self.assertEqual(
            covers.full_rule(None, self.scripts(), False, self.repo),
            "the files changed are unknown",
        )


class Orphans(Repo):
    """Every scripts/generative/*.txt is picked by check_all.py, included by another script, or says what runs it in
    a "# runs-via:" line (#466)."""

    def orphans(self):
        return covers.orphans(self.here)

    def test_the_fixtures_scripts_and_includes_have_no_orphan(self):
        self.assertEqual(self.orphans(), [])

    def test_a_script_check_all_doesnt_pick_is_one(self):
        self.write(G + "song_x.txt", "# needs: card fixtures/x\nstart_state\n")
        self.assertEqual(self.orphans(), ["song_x.txt"])

    def test_an_include_nothing_includes_is_one(self):
        self.write(G + "perform_body.txt", "button 1 2 down\n")
        self.assertEqual(self.orphans(), ["perform_body.txt"])

    def test_an_include_of_an_orphan_counts_as_included(self):
        # the orphan is named; what it includes is not named again
        self.write(G + "capture_x.txt", "start_state\ninclude capture_body.txt\n")
        self.write(G + "capture_body.txt", "button 1 2 down\n")
        self.assertEqual(self.orphans(), ["capture_x.txt"])

    def test_a_runs_via_line_declares_it(self):
        self.write(G + "golden_x.txt", "# runs-via: gen_emu.py golden\nstart_state\n")
        self.assertEqual(self.orphans(), [])

    def test_a_runs_via_line_needs_a_command(self):
        self.write(G + "golden_x.txt", "# runs-via:\nstart_state\n")
        self.assertEqual(self.orphans(), ["golden_x.txt"])

    def test_a_script_check_all_lists_as_not_run_is_declared(self):
        # its "# needs:" line names a song: check_all.py's "not run" row lists it
        self.write(G + "perform_song_y.txt", "# needs: SONGS/Y.XML\nstart_state\n")
        self.assertEqual(self.orphans(), [])

    def test_check_names_each_orphan_and_fails(self):
        subprocess.run(["git", "init", "-q"], cwd=self.repo, check=True)
        self.write(G + "song_x.txt", "start_state\n")
        subprocess.run(["git", "add", "-A"], cwd=self.repo, check=True)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = covers.check(covers.all_scripts(self.here), self.repo)
        self.assertEqual(code, 1)
        self.assertIn(
            "song_x.txt: check_all.py doesn't pick it and no script includes it: name it perform_*.txt or "
            'compose_*.txt, or give it a "# runs-via: COMMAND" line',
            out.getvalue(),
        )
        self.assertIn("1 orphan scripts", out.getvalue())


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
        # a tree with both kinds of %s sender, each with its BUILDERS or RELAYS entry, as the firmware has
        self.write(
            "src/deluge/t.cpp", 'TEST_EVENT("gen_transition_preload %s", text);\n'
        )
        self.write(
            "src/deluge/generative/firmware/sound_swaps.cpp",
            'TEST_EVENT("%s", text);\n',
        )
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


class BuilderGapsGuard(Repo):
    """builder_gaps() when the grep finds no %s sender at all: a broken pattern or grep, not a clean tree."""

    def setUp(self):
        super().setUp()
        self.write("src/deluge/x.cpp", 'TEST_EVENT("gen_beta n=%d", n);\n')
        subprocess.run(["git", "init", "-q"], cwd=self.repo, check=True)

    def test_says_so(self):
        gaps = covers.builder_gaps(self.repo, builders={}, relays={})
        self.assertEqual(len(gaps), 2, gaps)
        self.assertIn('no TEST_EVENT("gen_x ... %s")', gaps[0])
        self.assertIn('no TEST_EVENT("%s"', gaps[1])


class CheckGrepsOnce(unittest.TestCase):
    def test_check_greps_src_once(self):
        with (
            mock.patch.object(covers, "_grep_src", wraps=covers._grep_src) as grep,
            contextlib.redirect_stdout(io.StringIO()),
        ):
            covers.check([])
        self.assertEqual(grep.call_count, 1)


class TimingExpects(Repo):
    """An expect that pins a figure delugemu's speed decides must be hardware_only (#417)."""

    def test_a_pinned_timing_field_is_named(self):
        for text, field in (
            (r"/^gen_h16 culls=\d+ culled=\S+ voices=[1-9]/", "voices"),
            ("gen_h16 culls=0 culled=-", "culls"),
            ("/^gen_ext_tempo bpm=(119|120|121)$/", "bpm"),
            (r"/^gen_midi_feedback .* bytes_per_s=\d{4,} /", "bytes_per_s"),
            ("/^gen_h1 steps=8 mean_ns=1[0-9]{5} /", "mean_ns"),
            (
                "/^gen_h2 ramps=1 moved=1 steps=4 params=2 mean_ns=[0-9]{1,5} /",
                "mean_ns",
            ),
            ("gen_audio calls=10 underruns=0", "underruns"),
            (r"/^gen_audio_late task=\S+ .* run_max_us=[0-9]{1,3}$/", "run_max_us"),
            ("gen_audio_ui_timer timer=1 over_2ms=0", "over_2ms"),
            ("/^gen_transition start style=1 us=[0-9]{1,4} /", "us"),
            (
                "/^gen_arcplan first=0 us=(\\d{1,3})$/",
                "us",
            ),  # a group around a pinned value pins it
            ("gen_log_piece pieces=1 header=0 used=12 full=0 us=5", "us"),
        ):
            with self.subTest(text=text):
                self.assertEqual(covers.timing_field(text), field)

    def test_a_field_any_value_matches_pins_nothing(self):
        for text in (
            "gen_h16 culls=",
            r"/^gen_h16 culls=\d+ culled=\S+ voices=\d+ render_max_us=\d+/",
            "gen_h1 steps=",
            "/^gen_h1 /",
            r"/^gen_midi_feedback ccs=[1-9]\d* /",
            "gen_ext_tempo",
            "gen_phrase voices=3",  # not a timing kind
            "/^gen_h16 culls=[0-9]+ culled=[0-9]* voices=-?[0-9]+/",
            r"/^gen_h1 steps=8 mean_ns=(\d+) max_ns=(?P<max>\d+)/",  # a capture group around any value
            "/^gen_transition start style=1 us=([0-9]+) /",
        ):
            with self.subTest(text=text):
                self.assertIsNone(covers.timing_field(text))

    def test_unmarked_lists_each_such_expect_without_the_prefix(self):
        self.write(
            G + "perform_b.txt",
            "# covers: src/deluge/b_*.cpp\nstart_state\n"
            "expect event /^gen_h16 culls=\\d+ culled=\\S+ voices=[1-9]/ within=20\n"
            "hardware_only expect event /^gen_h16 culls=\\d+ culled=\\S+ voices=[1-9]/ within=20\n"
            "hardware_only expect no_event /^gen_midi_feedback .* bytes_per_s=\\d{4,} / since=mark within=1\n"
            "expect event gen_h16 culls= within=20\n",
        )
        self.assertEqual(
            covers.unmarked_timing([self.path("perform_b.txt")]),
            [
                (
                    "perform_b.txt:3",
                    r"/^gen_h16 culls=\d+ culled=\S+ voices=[1-9]/",
                    "voices",
                )
            ],
        )

    def test_check_fails_on_one(self):
        subprocess.run(["git", "init", "-q"], cwd=self.repo, check=True)
        self.write("src/deluge/b_one.cpp", 'TEST_EVENT("gen_ext_tempo bpm=%ld", t);\n')
        self.write(
            G + "perform_b.txt",
            "# covers: src/deluge/b_*.cpp\nstart_state\nexpect event gen_ext_tempo bpm=120 within=10\n",
        )
        subprocess.run(["git", "add", "-A"], cwd=self.repo, check=True)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = covers.check([self.path("perform_b.txt")], self.repo)
        self.assertEqual(code, 1)
        self.assertIn(
            "perform_b.txt:3: expect 'gen_ext_tempo bpm=120' pins bpm=, which delugemu's speed decides: make it"
            " hardware_only expect",
            out.getvalue(),
        )
        self.assertIn("1 timing expects not hardware_only", out.getvalue())


class ExpectLine(unittest.TestCase):
    def test_a_hardware_only_expect_is_an_expect(self):
        self.assertEqual(
            covers.EXPECT_LINE.match(
                "hardware_only expect event gen_x a=1 within=1"
            ).group(1),
            "gen_x a=1",
        )

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

    def test_every_timing_expect_is_hardware_only(self):
        self.assertEqual(covers.unmarked_timing(covers.all_scripts()), [])

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
