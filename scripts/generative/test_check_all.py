"""Tests for check_all.py's table notes.

python3 scripts/generative/test_check_all.py
"""

import contextlib
import io
import os
import sys
import tempfile
import threading
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_all

CYCLES = (
    "[gen-emu]   cycle 1: internal 1000 -> 680 (-320 B), external 5000 -> 5000 (+0 B) [warm-up, not checked]\n"
    "[gen-emu]   cycle 2: internal 680 -> 680 (+0 B), external 5000 -> 5000 (+0 B)\n"
)


class NeedsLines(unittest.TestCase):
    """A "# needs: card DIR" script runs on its own card (#277); other "# needs:" scripts are skipped."""

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.here = tmp.name
        scripts = {
            "test_port_smoke.txt": "start_state\n",
            "perform_a.txt": "start_state\n",
            "perform_card.txt": "# needs: card fixtures/perform_card (its own card)\nstart_state\n",
            "perform_song.txt": "# needs: SONGS/Ttt.XML on the card\nstart_state\n",
            "perform_open.txt": "button 4 3 down\n",
        }
        for name, text in scripts.items():
            with open(os.path.join(self.here, name), "w") as fh:
                fh.write(text)
        patcher = mock.patch.object(check_all, "HERE", self.here)
        patcher.start()
        self.addCleanup(patcher.stop)

    def path(self, name):
        return os.path.join(self.here, name)

    def test_a_card_overlay_script_runs_and_the_others_with_needs_are_skipped(self):
        scripts, skipped = check_all.covers.probe_scripts(check_all.HERE)
        self.assertEqual(
            scripts,
            [
                self.path(n)
                for n in ("test_port_smoke.txt", "perform_a.txt", "perform_card.txt")
            ],
        )
        self.assertEqual(skipped, ["perform_song.txt"])

    def test_reads_each_script_once(self):
        real_open = open
        with mock.patch("builtins.open", side_effect=real_open) as opened:
            check_all.covers.probe_scripts(check_all.HERE)
        paths = [call.args[0] for call in opened.call_args_list]
        self.assertEqual(paths.count(self.path("perform_card.txt")), 1, paths)


class Plan(unittest.TestCase):
    """What a run checks for the files changed since the base (#314): the unit tests only for a change under
    scripts/, pack/ or src/deluge/generative/, the firmware builds and delugemu (smoke and the leak check with them)
    only for a change to a firmware input, the emulator checks' own code or a probe script, the probe scripts by their
    covers lines."""

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.repo = tmp.name
        self.here = os.path.join(self.repo, "scripts", "generative")
        os.makedirs(self.here)
        scripts = {
            "test_port_smoke.txt": "start_state\n",
            "perform_a.txt": "# covers: src/a.cpp\nstart_state\n",
            "perform_b.txt": "# covers: src/b.cpp\nstart_state\n",
            "perform_mem_cycle.txt": "# needs: gen_emu.py leak\nstart_state\ninclude perform_cycle_inc.txt\n",
            "perform_cycle_inc.txt": "button 2 2 down\n",
            "perform_open.txt": "button 1 1 down\n",
            # the emulator checks' modules, by their imports (covers.module_files())
            "gen_emu.py": "import deluge_probe\nimport test_port_emu\nCYCLE = 'perform_mem_cycle.txt'\n",
            "test_port_emu.py": "import os\nimport deluge_probe\nimport fat_image\n",
            "deluge_probe.py": 'import os\nLEAVE = "perform_leave.txt"\n',
            "perform_leave.txt": "button 1 1 down\n",
            "fat_image.py": "import struct\n",
            "check_all.py": "import covers\n",
            "covers.py": "import os\n",
        }
        scripts["test_port_smoke.txt"] = "start_state\ninclude perform_open.txt\n"
        for name, text in scripts.items():
            with open(os.path.join(self.here, name), "w") as fh:
                fh.write(text)
        for name, value in (("HERE", self.here), ("REPO", self.repo)):
            patcher = mock.patch.object(check_all, name, value)
            patcher.start()
            self.addCleanup(patcher.stop)
        patcher = mock.patch.object(check_all.covers, "REPO", self.repo)
        patcher.start()
        self.addCleanup(patcher.stop)

    def plan(self, changed, full=False, given=None):
        return check_all.plan(changed, "origin/m2", full, given)

    def names(self, plan):
        return [os.path.basename(s) for s in plan.scripts]

    def test_a_docs_change_skips_the_unit_tests_the_builds_and_delugemu(self):
        plan = self.plan(["docs/x.md", "tests/CMakeLists.txt"])
        self.assertIn("scripts/", plan.skip_unit_tests)
        self.assertIn("no firmware input", plan.skip_firmware)
        self.assertEqual(plan.scripts, [])

    def test_a_scripts_change_runs_the_unit_tests_and_not_the_firmware(self):
        plan = self.plan(["scripts/generative/make_pack.py"])
        self.assertIsNone(plan.skip_unit_tests)
        self.assertIsNotNone(plan.skip_firmware)

    def test_a_pack_or_generative_source_change_runs_the_unit_tests(self):
        self.assertIsNone(self.plan(["pack/SYNTHS/x.XML"]).skip_unit_tests)
        self.assertIsNone(
            self.plan(["src/deluge/generative/core/x.cpp"]).skip_unit_tests
        )

    def test_every_firmware_input_builds_and_runs_smoke_and_the_smoke_script(self):
        for path in (
            "src/deluge/dsp/reverb.cpp",
            "lib/CMakeLists.txt",
            "CMakeLists.txt",
            "linker_script_rz_a1l.ld",
            "toolchain/REQUIRED_VERSION",
            "scripts/toolchain/requirements.txt",
            "scripts/tasks/task-build.py",
            "scripts/tasks/task-configure.py",
            "scripts/cmake/CMakeToolchainDeluge.cmake",
            "dbt",
        ):
            with self.subTest(path=path):
                plan = self.plan([path])
                self.assertIsNone(plan.skip_firmware)
                self.assertEqual(self.names(plan), ["test_port_smoke.txt"])

    def test_a_firmware_change_runs_the_leg_even_if_every_script_has_covers_lines(self):
        # not only because test_port_smoke.txt has none: the firmware itself is reason enough
        with open(os.path.join(self.here, "test_port_smoke.txt"), "w") as fh:
            fh.write("# covers: src/never.cpp\nstart_state\n")
        plan = self.plan(["src/deluge/dsp/reverb.cpp"])
        self.assertIsNone(plan.skip_firmware)
        self.assertEqual(self.names(plan), ["test_port_smoke.txt"])

    def test_the_smoke_script_or_its_include_alone_runs_it(self):
        for path in (
            "scripts/generative/test_port_smoke.txt",
            "scripts/generative/perform_open.txt",
        ):
            with self.subTest(path=path):
                plan = self.plan([path])
                self.assertIsNone(plan.skip_firmware)
                self.assertEqual(self.names(plan)[0], "test_port_smoke.txt")

    def test_the_emulator_checks_own_code_runs_the_firmware_leg(self):
        plan = self.plan(["scripts/generative/gen_emu.py"])
        self.assertIsNone(plan.skip_firmware)
        self.assertEqual(self.names(plan), ["test_port_smoke.txt"])

    def test_the_leak_checks_cycle_script_or_its_include_runs_the_firmware_leg(self):
        for path in ("perform_mem_cycle.txt", "perform_cycle_inc.txt"):
            with self.subTest(path=path):
                plan = self.plan(["scripts/generative/" + path])
                self.assertIsNone(plan.skip_firmware)
                self.assertEqual(self.names(plan), ["test_port_smoke.txt"])

    def test_the_script_start_state_runs_runs_every_script(self):
        plan = self.plan(["scripts/generative/perform_leave.txt"])
        self.assertEqual(
            self.names(plan), ["test_port_smoke.txt", "perform_a.txt", "perform_b.txt"]
        )

    def test_check_all_and_covers_run_the_firmware_leg(self):
        for path in ("scripts/generative/check_all.py", "scripts/generative/covers.py"):
            with self.subTest(path=path):
                self.assertIsNone(self.plan([path]).skip_firmware)

    def test_a_module_the_script_runner_imports_runs_every_script(self):
        plan = self.plan(["scripts/generative/fat_image.py"])
        self.assertEqual(
            self.names(plan), ["test_port_smoke.txt", "perform_a.txt", "perform_b.txt"]
        )

    def test_a_narrow_src_change_runs_the_smoke_script_and_the_scripts_that_cover_it(
        self,
    ):
        plan = self.plan(["src/b.cpp"])
        self.assertIsNone(plan.skip_firmware)
        self.assertEqual(self.names(plan), ["test_port_smoke.txt", "perform_b.txt"])
        self.assertEqual(
            [os.path.basename(s) for s in plan.not_picked], ["perform_a.txt"]
        )

    def test_a_src_change_no_script_covers_still_builds_and_runs_the_smoke_script(self):
        plan = self.plan(["src/other.cpp"])
        self.assertIsNone(plan.skip_firmware)
        self.assertEqual(self.names(plan), ["test_port_smoke.txt"])

    def test_a_changed_probe_script_runs_with_no_src_change(self):
        plan = self.plan(["scripts/generative/perform_a.txt"])
        self.assertIsNone(plan.skip_firmware)
        self.assertEqual(self.names(plan), ["test_port_smoke.txt", "perform_a.txt"])

    def test_full_runs_everything_whatever_changed(self):
        plan = self.plan(["docs/x.md"], full=True)
        self.assertIsNone(plan.skip_unit_tests)
        self.assertIsNone(plan.skip_firmware)
        self.assertEqual(
            self.names(plan), ["test_port_smoke.txt", "perform_a.txt", "perform_b.txt"]
        )

    def test_no_merge_base_runs_everything(self):
        plan = self.plan(None)
        self.assertIsNone(plan.skip_unit_tests)
        self.assertEqual(len(plan.scripts), 3)

    def test_scripts_given_run_as_given_with_the_firmware(self):
        given = [os.path.join(self.here, "perform_a.txt")]
        plan = self.plan(["docs/x.md"], given=given)
        self.assertIsNone(plan.skip_firmware)
        self.assertEqual(plan.scripts, given)


class LongestFirst(unittest.TestCase):
    def test_orders_by_recorded_cost_then_listed_cost(self):
        jobs = [("a", [], None), ("b", [], None), ("c", [], None)]
        recorded = {"a": 10.0, "b": 50.0}
        listed = {"c": 30.0, "a": 999.0}
        self.assertEqual(
            [j[0] for j in check_all.longest_first(jobs, recorded, listed)],
            ["b", "c", "a"],
        )

    def test_an_unknown_job_starts_right_after_the_longest_listed_one(self):
        # not infinite: on a fresh worktree every job is unknown but the listed ones, and the longest listed one
        # must not wait behind a dozen short unknown ones; a new script may be long, so it starts next
        jobs = [("a_short", [], None), ("clip", [], None), ("browser", [], None)]
        listed = {"browser": 240.0, "clip": 100.0}
        self.assertEqual(
            [j[0] for j in check_all.longest_first(jobs, {}, listed)],
            ["browser", "a_short", "clip"],
        )

    def test_costs_round_trip_and_a_bad_file_reads_as_none(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "costs.json")
            self.assertEqual(check_all.read_costs(path), {})
            check_all.write_costs(
                path, [("x", "PASS", 12.5, "", ""), ("y", "FAIL", 3.0, "", "")]
            )
            # a failed run's time says nothing about the job's length: not recorded
            self.assertEqual(check_all.read_costs(path), {"x": 12.5})
            check_all.write_costs(path, [("y", "PASS", 4.0, "", "")])
            self.assertEqual(check_all.read_costs(path), {"x": 12.5, "y": 4.0})
            with open(path, "w") as fh:
                fh.write("{")
            self.assertEqual(check_all.read_costs(path), {})
            with open(path, "w") as fh:
                fh.write('{"x": null}')  # float(None): TypeError
            self.assertEqual(check_all.read_costs(path), {})


class LeakDetail(unittest.TestCase):
    def test_a_pass_lists_each_cycles_deltas(self):
        text = CYCLES + "[gen-emu] leak check: PASS (logs in x)\n"
        self.assertEqual(
            check_all.leak_detail(text),
            "internal/external B per cycle: c1 -320/+0, c2 +0/+0",
        )

    def test_counts_every_problem_the_check_counted_not_only_cycle_ones(self):
        text = (
            CYCLES
            + "[gen-emu] FAIL only 2 of 4 enter/leave cycles completed (see the script output)\n"
            + "[gen-emu] FAIL 3 expects failed in the cycles\n"
            + "[gen-emu] leak check: FAIL, 2 problems (logs in x)\n"
        )
        self.assertEqual(
            check_all.leak_detail(text),
            "2 problems; internal/external B per cycle: c1 -320/+0, c2 +0/+0",
        )

    def test_a_check_that_never_got_to_the_cycles_says_why(self):
        text = "[gen-emu] FAIL leak check: no reply to test port command 0x11\n"
        self.assertEqual(
            check_all.leak_detail(text), "no reply to test port command 0x11"
        )


class LoadWarning(unittest.TestCase):
    def test_a_host_busier_than_its_cores_warns(self):
        self.assertIn(
            "load average is 30 on 10 cores", check_all.load_warning(30.2, 10)
        )

    def test_a_quiet_host_doesnt(self):
        self.assertIsNone(check_all.load_warning(4.0, 10))

    def test_the_summary_says_it_once(self):
        self.assertEqual(check_all.summary_load_warning("at start", "at end"), "at end")
        self.assertEqual(check_all.summary_load_warning("at start", None), "at start")
        self.assertIsNone(check_all.summary_load_warning(None, None))


class ArmTypesDetail(unittest.TestCase):
    def test_the_count_line(self):
        self.assertEqual(
            check_all.ARM_TYPES_DETAIL("x\n90 files on ARM, 0 with errors\n"),
            "90 files on ARM, 0 with errors",
        )

    def test_no_toolchain_shows_how_to_get_one(self):
        text = (
            "arm_types: no ARM toolchain yet: run any ./dbt command once to fetch it\n"
        )
        self.assertEqual(check_all.ARM_TYPES_DETAIL(text), text.strip())


class StepSectionDetail(unittest.TestCase):
    def test_gives_the_sections_size_and_the_targets_outside(self):
        text = (
            ".text.generative_step: 0x201699a0..0x201727b4 (36,372 B)\n"
            "deluge::generative targets outside it: 0\n"
            "other targets outside it (allowed): __divsi3, memmove\n"
        )
        self.assertEqual(check_all.step_section_detail(text), "36,372 B, 0 outside")

    def test_a_run_that_stopped_early_gives_nothing(self):
        self.assertEqual(
            check_all.step_section_detail("FileNotFoundError: deluge.map\n"), ""
        )


class FailedExpects(unittest.TestCase):
    def test_picks_each_fail_line_with_the_got_lines_under_it(self):
        lines = [
            "[test-port-emu] x.txt: PASS line 3: expect playing=true",
            "[test-port-emu] x.txt: FAIL line 4: expect event gen_edit control=none",
            "[test-port-emu] x.txt:   got no gen_edit event",
            "[test-port-emu] x.txt: PASS line 5: expect voices=0",
            "[test-port-emu] x.txt: FAIL line 9: expect no_event gen_midi_feedback within=3",
            "[test-port-emu] x.txt:   got 2.5: gen_midi_feedback ccs=1",
            "[test-port-emu]   x.txt: 9 expects, 2 failed",
        ]
        self.assertEqual(
            check_all.failed_expects(lines), [lines[1], lines[2], lines[4], lines[5]]
        )

    def test_picks_the_end_check_for_dropped_events(self):
        line = "[test-port-emu] x.txt: FAIL end: 2 test events dropped since start_state: the firmware's event queue"
        self.assertEqual(check_all.failed_expects([line]), [line])

    def test_a_passing_log_picks_nothing(self):
        self.assertEqual(
            check_all.failed_expects(
                ["[test-port-emu] x.txt: PASS line 3: expect playing=true"]
            ),
            [],
        )


class RunTogether(unittest.TestCase):
    def setUp(self):
        logs = tempfile.TemporaryDirectory()
        self.addCleanup(logs.cleanup)
        patch = mock.patch.object(check_all, "LOGS", logs.name)
        patch.start()
        self.addCleanup(patch.stop)

    def run_together(self, jobs, workers):
        checks = check_all.Checks()
        with contextlib.redirect_stdout(io.StringIO()):
            checks.run_together(jobs, workers)
        return checks

    def test_rows_keep_the_jobs_order_whichever_ends_first(self):
        slow = [sys.executable, "-c", "import time; time.sleep(0.5)"]
        fast = [sys.executable, "-c", "raise SystemExit(1)"]
        checks = self.run_together([("slow", slow, None), ("fast", fast, None)], 2)
        self.assertEqual(
            [row[:2] for row in checks.rows], [("slow", "PASS"), ("fast", "FAIL")]
        )

    def test_a_job_that_fails_beside_others_runs_again_alone_and_that_result_counts(
        self,
    ):
        # Fails on its first run and passes on its second: a marker file tells them apart.
        marker = os.path.join(check_all.LOGS, "ran-once")
        flaky = [
            sys.executable,
            "-c",
            (
                "import os, sys; p = sys.argv[1]; seen = os.path.exists(p); open(p, 'w').close(); "
                "print('second' if seen else 'first'); sys.exit(0 if seen else 1)"
            ),
            marker,
        ]
        broken = [sys.executable, "-c", "raise SystemExit(1)"]
        checks = self.run_together(
            [("flaky", flaky, str.strip), ("broken", broken, None)], 2
        )
        self.assertEqual(
            [row[:2] for row in checks.rows], [("flaky", "PASS"), ("broken", "FAIL")]
        )
        self.assertEqual(
            checks.rows[0][3], "second; passed alone after failing beside the others"
        )
        with open(os.path.join(check_all.LOGS, "flaky.together.log")) as fh:
            self.assertEqual(fh.read().strip(), "first")
        self.assertEqual(checks.passed_alone(), 1)

    def test_a_job_that_passed_alone_records_the_alone_runs_seconds_as_its_cost(self):
        # The run beside the others took 0.6 s more (a load that made it fail); the table shows both, the cost the
        # alone run only.
        marker = os.path.join(check_all.LOGS, "ran-once")
        flaky = [
            sys.executable,
            "-c",
            (
                "import os, sys, time; p = sys.argv[1]; seen = os.path.exists(p); open(p, 'w').close(); "
                "seen or time.sleep(0.6); sys.exit(0 if seen else 1)"
            ),
            marker,
        ]
        fine = [sys.executable, "-c", "pass"]
        checks = self.run_together([("flaky", flaky, None), ("fine", fine, None)], 2)
        costs = {name: seconds for name, _, seconds, _, _ in checks.cost_rows}
        # the row is both runs, the cost the alone one: their difference is the first run, which slept 0.6 s, however
        # slow the host
        self.assertGreaterEqual(checks.rows[0][2] - costs["flaky"], 0.6)
        self.assertIn("fine", costs)

    def test_an_earlier_runs_together_logs_are_cleared(self):
        stale = os.path.join(check_all.LOGS, "old.together.log")
        open(stale, "w").close()
        check_all.Checks()
        self.assertFalse(os.path.exists(stale))

    def test_one_at_a_time_runs_nothing_twice(self):
        broken = [sys.executable, "-c", "print('x'); raise SystemExit(1)"]
        checks = self.run_together([("a", broken, None), ("b", broken, None)], 1)
        self.assertEqual([row[1] for row in checks.rows], ["FAIL", "FAIL"])
        self.assertFalse(os.path.exists(os.path.join(check_all.LOGS, "a.together.log")))

    def test_starts_the_jobs_in_the_order_given_and_rows_keep_it(self):
        started = []
        real_run = check_all.subprocess.run

        def run(cmd, **kwargs):
            started.append(cmd[-1])
            return real_run(cmd, **kwargs)

        jobs = [(n, [sys.executable, "-c", "pass", n], None) for n in ("a", "b", "c")]
        checks = check_all.Checks()
        with (
            mock.patch.object(check_all.subprocess, "run", run),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            checks.run_together(jobs, 1, start_order=[jobs[2], jobs[0], jobs[1]])
        self.assertEqual(started, ["c", "a", "b"])
        self.assertEqual([row[0] for row in checks.rows], ["a", "b", "c"])

    def test_runs_the_jobs_side_by_side(self):
        # Each job waits for a partner before it starts, so one at a time would time out.
        meet = threading.Barrier(2, timeout=10)
        real_run = check_all.subprocess.run

        def run(cmd, **kwargs):
            meet.wait()
            return real_run(cmd, **kwargs)

        job = [sys.executable, "-c", "pass"]
        with mock.patch.object(check_all.subprocess, "run", run):
            checks = self.run_together([(str(n), job, None) for n in range(4)], 2)
        self.assertEqual([row[1] for row in checks.rows], ["PASS"] * 4)


class HardwareOnlySkips(unittest.TestCase):
    def test_a_probe_scripts_note_keeps_its_skip_count(self):
        text = "x.txt: 3 expects, 0 failed\n[test-port-emu] 3 checks, 0 failed, 2 hardware-only skipped; logs in o\n"
        self.assertEqual(
            check_all.SCRIPT_RESULT(text), "3 checks, 0 failed, 2 hardware-only skipped"
        )
        self.assertEqual(
            check_all.SCRIPT_RESULT("4 checks, 1 failed; logs in o"),
            "4 checks, 1 failed",
        )

    def test_the_table_counts_them_under_its_rows(self):
        logs = tempfile.TemporaryDirectory()
        self.addCleanup(logs.cleanup)
        with mock.patch.object(check_all, "LOGS", logs.name):
            checks = check_all.Checks()
        checks.rows = [
            (
                "delugemu a.txt",
                "PASS",
                1.0,
                "3 checks, 0 failed, 2 hardware-only skipped",
                "",
            ),
            ("delugemu b.txt", "PASS", 1.0, "4 checks, 0 failed", ""),
            (
                "delugemu c.txt",
                "FAIL",
                1.0,
                "5 checks, 1 failed, 3 hardware-only skipped",
                "",
            ),
        ]
        printed = io.StringIO()
        with contextlib.redirect_stdout(printed):
            self.assertEqual(checks.table(), 1)
        self.assertIn(
            "hardware-only expects skipped under delugemu: 5, in 2 scripts (they run on the Deluge)",
            printed.getvalue(),
        )

    def test_with_none_the_table_says_nothing_of_them(self):
        logs = tempfile.TemporaryDirectory()
        self.addCleanup(logs.cleanup)
        with mock.patch.object(check_all, "LOGS", logs.name):
            checks = check_all.Checks()
        checks.rows = [("delugemu b.txt", "PASS", 1.0, "4 checks, 0 failed", "")]
        printed = io.StringIO()
        with contextlib.redirect_stdout(printed):
            checks.table()
        self.assertNotIn("hardware-only", printed.getvalue())


class SlotWaits(unittest.TestCase):
    """A run that waited for a real-time emulator slot (emu_slots.py, #417) says so in its row, and the table sums
    the waits."""

    def setUp(self):
        logs = tempfile.TemporaryDirectory()
        self.addCleanup(logs.cleanup)
        patch = mock.patch.object(check_all, "LOGS", logs.name)
        patch.start()
        self.addCleanup(patch.stop)
        self.checks = check_all.Checks()

    def test_a_row_that_waited_says_how_long_after_its_note(self):
        cmd = [
            sys.executable,
            "-c",
            (
                "print('[test-port-emu] waited 42 s for a real-time emulator slot (slot 0 of 6)');"
                "print('3 checks, 0 failed; logs in o')"
            ),
        ]
        with contextlib.redirect_stdout(io.StringIO()):
            self.checks.run("delugemu a.txt", cmd, check_all.SCRIPT_RESULT)
        self.assertEqual(
            self.checks.rows[0][3],
            "3 checks, 0 failed; waited 42 s for an emulator slot",
        )

    def test_a_row_with_no_detail_still_says_it_waited(self):
        cmd = [
            sys.executable,
            "-c",
            "print('waited 5 s for a real-time emulator slot (slot 1 of 2)')",
        ]
        with contextlib.redirect_stdout(io.StringIO()):
            self.checks.run("leak check", cmd)
        self.assertEqual(self.checks.rows[0][3], "waited 5 s for an emulator slot")

    def test_a_row_that_didnt_wait_says_nothing_of_it(self):
        cmd = [sys.executable, "-c", "print('3 checks, 0 failed')"]
        with contextlib.redirect_stdout(io.StringIO()):
            self.checks.run("delugemu a.txt", cmd, check_all.SCRIPT_RESULT)
        self.assertEqual(self.checks.rows[0][3], "3 checks, 0 failed")

    def test_the_table_sums_the_waits_under_its_rows(self):
        self.checks.rows = [
            (
                "delugemu a.txt",
                "PASS",
                1.0,
                "3 checks, 0 failed; waited 42 s for an emulator slot",
                "",
            ),
            ("delugemu b.txt", "PASS", 1.0, "4 checks, 0 failed", ""),
            ("leak check", "PASS", 1.0, "waited 8 s for an emulator slot", ""),
        ]
        printed = io.StringIO()
        with (
            contextlib.redirect_stdout(printed),
            mock.patch.dict(os.environ, {"DELUGE_EMU_SLOTS": "4"}),
        ):
            checks_failed = self.checks.table()
        self.assertEqual(checks_failed, 0)
        self.assertIn(
            "waited for real-time emulator slots: 50 s in total, in 2 runs (pool of 4, DELUGE_EMU_SLOTS)",
            printed.getvalue(),
        )

    def test_with_no_waits_the_table_says_nothing_of_them(self):
        self.checks.rows = [("delugemu b.txt", "PASS", 1.0, "4 checks, 0 failed", "")]
        printed = io.StringIO()
        with contextlib.redirect_stdout(printed):
            self.checks.table()
        self.assertNotIn("emulator slot", printed.getvalue())


class MergeRuleHeader(unittest.TestCase):
    """The table's header says which --full rule applied (covers.full_rule(), #417 slice 3)."""

    def setUp(self):
        logs = tempfile.TemporaryDirectory()
        self.addCleanup(logs.cleanup)
        with mock.patch.object(check_all, "LOGS", logs.name):
            self.checks = check_all.Checks()
        self.checks.rows = [("a", "PASS", 1.0, "", "")]

    def table(self):
        printed = io.StringIO()
        with contextlib.redirect_stdout(printed):
            self.checks.table()
        return printed.getvalue().strip().splitlines()

    def test_a_covers_pick_that_suffices(self):
        self.checks.merge_rule = check_all.merge_rule_line(None, full=False)
        self.assertEqual(self.table()[0], "merge rule: covers pick suffices")

    def test_a_change_that_needs_full_says_why(self):
        self.checks.merge_rule = check_all.merge_rule_line(
            "check_all.py changed", full=False
        )
        self.assertEqual(
            self.table()[0],
            "merge rule: needs --full: check_all.py changed (this run isn't --full: run it with --full before the merge)",
        )

    def test_the_base_generative_is_a_merge_into_it(self):
        with mock.patch.object(
            check_all.covers, "full_rule", return_value=None
        ) as rule:
            check_all.full_reason(["docs/x.md"], "origin/generative")
            self.assertTrue(rule.call_args.args[2])
            check_all.full_reason(["docs/x.md"], "origin/m4-capture")
            self.assertFalse(rule.call_args.args[2])
            self.assertEqual(rule.call_args.args[0], ["docs/x.md"])

    def test_no_merge_base_runs_everything_so_counts_as_a_full_run(self):
        # changed None: plan() runs every check and script, so nothing asks for another run with --full
        with mock.patch.object(
            check_all.covers, "full_rule", return_value="the files changed are unknown"
        ):
            reason, line, ran_full = check_all.merge_rule(
                None, "origin/m4-capture", full=False
            )
        self.assertTrue(ran_full)
        self.assertEqual(reason, "the files changed are unknown")
        self.assertEqual(
            line,
            "merge rule: needs --full: the files changed are unknown (this run is --full)",
        )
        self.assertEqual(check_all.full_run_note(reason, ran_full), "")

    def test_a_pick_run_that_needs_full_says_so_on_the_last_line(self):
        with mock.patch.object(check_all.covers, "full_rule", return_value="x changed"):
            reason, line, ran_full = check_all.merge_rule(
                ["x"], "origin/m4-capture", full=False
            )
        self.assertFalse(ran_full)
        self.assertIn("this run isn't --full", line)
        self.assertEqual(
            check_all.full_run_note(reason, ran_full),
            "; needs --full before the merge (x changed)",
        )
        with mock.patch.object(check_all.covers, "full_rule", return_value="x changed"):
            reason, line, ran_full = check_all.merge_rule(
                ["x"], "origin/m4-capture", full=True
            )
        self.assertEqual(check_all.full_run_note(reason, ran_full), "")

    def test_a_full_run_says_it_was_one(self):
        self.checks.merge_rule = check_all.merge_rule_line(
            "check_all.py changed", full=True
        )
        self.assertEqual(
            self.table()[0],
            "merge rule: needs --full: check_all.py changed (this run is --full)",
        )
        self.checks.merge_rule = check_all.merge_rule_line(None, full=True)
        self.assertEqual(
            self.table()[0], "merge rule: covers pick suffices (this run is --full)"
        )


class FindBase(unittest.TestCase):
    def setUp(self):
        logs = tempfile.TemporaryDirectory()
        self.addCleanup(logs.cleanup)
        patch = mock.patch.object(check_all, "LOGS", logs.name)
        patch.start()
        self.addCleanup(patch.stop)

    def find(self, given, found):
        checks = check_all.Checks()
        with (
            mock.patch.object(check_all.pr_base, "merge_base", lambda *a: found),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            check_all.find_base(given, checks)
        return [row[:2] for row in checks.rows]

    def test_a_given_base_that_cant_be_found_is_a_fail_row(self):
        missing = check_all.pr_base.Base(
            "origin/m9", None, None, "", "no origin/m9 to compare with (given): x"
        )
        self.assertEqual(self.find("m9", missing), [("base", "FAIL")])
        unrelated = check_all.pr_base.Base("origin/m9", "given", None, "", None)
        self.assertEqual(self.find("m9", unrelated), [("base", "FAIL")])

    def test_a_given_base_with_an_error_is_a_fail_row_even_with_a_merge_base(self):
        # origin/m9 resolved through a local branch or tag of that name: merge-base found something.
        shadowed = check_all.pr_base.Base(
            "origin/m9", None, None, "abc123", "no origin/m9 to compare with (given): x"
        )
        self.assertEqual(self.find("m9", shadowed), [("base", "FAIL")])

    def test_a_missing_default_base_adds_no_row(self):
        missing = check_all.pr_base.Base(
            "origin/generative", None, None, "", "no origin/generative"
        )
        self.assertEqual(self.find(None, missing), [])

    def test_a_found_base_adds_no_row(self):
        found = check_all.pr_base.Base("origin/m2", "given", None, "abc123", None)
        self.assertEqual(self.find("m2", found), [])


class SameNamed(unittest.TestCase):
    def test_names_given_twice_are_listed_whatever_their_directories(self):
        self.assertEqual(
            check_all.same_named(["a/x.txt", "b/x.txt", "a/y.txt"]), ["x.txt"]
        )

    def test_distinct_names_list_nothing(self):
        self.assertEqual(check_all.same_named(["a/x.txt", "a/y.txt"]), [])


class ResolveScriptTest(unittest.TestCase):
    def test_a_bare_name_is_found_in_scripts_generative(self):
        self.assertEqual(
            check_all.resolve_script("perform_homes.txt"),
            os.path.join(check_all.HERE, "perform_homes.txt"),
        )

    def test_a_path_is_taken_as_given(self):
        path = os.path.join(check_all.HERE, "perform_homes.txt")
        self.assertEqual(check_all.resolve_script(os.path.relpath(path)), path)

    def test_a_missing_script_stops_the_run_naming_it(self):
        with self.assertRaises(SystemExit) as stopped:
            check_all.resolve_script("perform_nothing.txt")
        self.assertIn("perform_nothing.txt", str(stopped.exception.code))


if __name__ == "__main__":
    unittest.main()
