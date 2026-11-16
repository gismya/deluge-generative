"""Unit tests for task-sizediff.py's base (./dbt sizediff --base): a branch name means origin's, fetched first, and a
base that can't be found stops the run with a message before anything is built.

    python3 scripts/tasks/test_task_sizediff.py
"""

import contextlib
import importlib
import io
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

sizediff = importlib.import_module("task-sizediff")


def git(cwd, *args):
    return subprocess.run(
        ["git", *args], cwd=cwd, check=True, capture_output=True, text=True
    ).stdout.strip()


class Base(unittest.TestCase):
    """A scratch clone whose local generative is stale: origin's moved on after the clone."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        origin = os.path.join(self.tmp.name, "origin")
        self.repo = os.path.join(self.tmp.name, "repo")
        os.makedirs(origin)
        git(origin, "init", "-q", "-b", "generative")
        for cwd in (origin,):
            for key, value in (
                ("user.email", "t@t"),
                ("user.name", "t"),
                ("commit.gpgsign", "false"),
            ):
                git(cwd, "config", key, value)
        git(origin, "commit", "-q", "--allow-empty", "-m", "first")
        git(self.tmp.name, "clone", "-q", origin, self.repo)
        for key, value in (
            ("user.email", "t@t"),
            ("user.name", "t"),
            ("commit.gpgsign", "false"),
        ):
            git(self.repo, "config", key, value)
        git(self.repo, "switch", "-q", "-c", "work")
        git(origin, "commit", "-q", "--allow-empty", "-m", "second")
        self.origin_head = git(origin, "rev-parse", "HEAD")
        git(self.repo, "fetch", "-q", "origin", "generative:refs/heads/scratch")
        git(self.repo, "merge", "-q", "scratch")

    def test_a_bare_branch_name_is_origins_fetched(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            found = sizediff.merge_base("generative", self.repo)
        self.assertEqual(found, (self.origin_head, "origin/generative", "given"))
        self.assertEqual(out.getvalue(), "")

    def test_a_branch_origin_hasnt_raises_a_message_not_a_traceback(self):
        with self.assertRaises(sizediff.pr_base.BaseError) as caught:
            sizediff.merge_base("m9", self.repo)
        self.assertIn("no origin/m9 to compare with", str(caught.exception))

    def test_main_stops_before_building_when_there_is_no_base(self):
        error = sizediff.pr_base.BaseError(
            "no origin/m9 to compare with (given): x", "origin/m9"
        )
        out = io.StringIO()
        with (
            mock.patch.object(sys, "argv", ["sizediff", "--base", "m9"]),
            mock.patch.object(sizediff.util, "get_git_root", lambda: self.repo),
            mock.patch.object(sizediff, "merge_base", mock.Mock(side_effect=error)),
            mock.patch.object(sizediff.importlib, "import_module") as build,
            contextlib.redirect_stdout(out),
        ):
            cwd = os.getcwd()
            self.addCleanup(os.chdir, cwd)
            code = sizediff.main()
        self.assertEqual(code, 1)
        self.assertIn("sizediff: no origin/m9 to compare with", out.getvalue())
        build.assert_not_called()


class Symbols(unittest.TestCase):
    def test_sized_symbols_by_type_and_name_with_spaces_kept(self):
        nm = (
            "         U memcpy\n"
            "20001001 00000010 T operator new(unsigned int)\n"
            "20002000 t no_size()\n"
            "20003000 00000004 b s\n"
            "20003004 00000008 b s\n"
        )
        with mock.patch.object(sizediff.util, "run_get_output", lambda cmd: nm):
            self.assertEqual(
                sizediff.symbols(Path("x.elf")),
                {("T", "operator new(unsigned int)"): [16], ("b", "s"): [4, 8]},
            )


if __name__ == "__main__":
    unittest.main()
