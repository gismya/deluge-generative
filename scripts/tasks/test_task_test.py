"""Unit tests for task-test.py (./dbt test): what --generator builds and runs, with cmake, ctest and the goldens'
version check faked.

    python3 scripts/tasks/test_task_test.py
"""

import contextlib
import importlib
import io
import subprocess
import sys
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

task_test = importlib.import_module("task-test")


class Generator(unittest.TestCase):
    def run_task(self, *argv, versions=0, goldens=("old", "new")):
        """main() with argv; returns (exit code, commands run, stderr). The goldens read `goldens` in turn."""
        commands = []

        def run(cmd, **kwargs):
            commands.append(cmd)
            code = versions if "goldens.py" in " ".join(cmd) else 0
            return subprocess.CompletedProcess(cmd, code)

        reads = iter(goldens)
        err = io.StringIO()
        with (
            mock.patch.object(sys, "argv", ["test", *argv]),
            mock.patch.object(task_test.subprocess, "run", run),
            mock.patch.object(task_test.util, "get_git_root", lambda: "."),
            mock.patch.object(task_test.os, "chdir", lambda path: None),
            mock.patch.object(task_test.os.path, "exists", lambda path: True),
            mock.patch.object(task_test, "golden_files", lambda: next(reads)),
            contextlib.redirect_stderr(err),
        ):
            code = task_test.main()
        return code, commands, err.getvalue()

    def built(self, commands):
        build = next(c for c in commands if c[:2] == ["cmake", "--build"])
        return build[build.index("--target") + 1 :]

    def test_plain_generator_builds_the_specs_and_the_render_cli(self):
        _, commands, _ = self.run_task("--generator")
        self.assertEqual(
            self.built(commands), ["generator_specs", "generator_render_cli"]
        )

    def test_update_goldens_builds_the_render_cli_too(self):
        _, commands, _ = self.run_task("--update-goldens")
        self.assertEqual(
            self.built(commands), ["generator_specs", "generator_render_cli"]
        )

    def test_sanitize_builds_only_the_specs(self):
        for flags in (
            ["--generator", "--sanitize"],
            ["--update-goldens", "--sanitize"],
        ):
            with self.subTest(flags=flags):
                _, commands, _ = self.run_task(*flags)
                self.assertEqual(self.built(commands), ["generator_specs"])

    def test_the_specs_run_by_label(self):
        _, commands, _ = self.run_task("--generator")
        self.assertEqual(commands[-1][-2:], ["-L", "generator"])

    def test_update_goldens_runs_golden_spec_alone_then_the_version_check(self):
        code, commands, err = self.run_task("--update-goldens")
        self.assertEqual(code, 0)
        self.assertIn("-R", commands[-2])
        self.assertIn("^golden_spec$", commands[-2])
        self.assertNotIn("-L", commands[-2])
        self.assertTrue(commands[-1][-2].endswith("goldens.py"))
        self.assertEqual(err, "")

    def test_a_refused_update_says_the_goldens_are_written_but_need_the_bump(self):
        code, _, err = self.run_task("--update-goldens", versions=1)
        self.assertEqual(code, 1)
        self.assertIn("The goldens are written", err)
        self.assertIn("needs the bump before you commit", err)


if __name__ == "__main__":
    unittest.main()
