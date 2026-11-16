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
