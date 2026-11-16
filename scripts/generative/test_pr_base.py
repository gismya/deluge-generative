"""Tests for pr_base.py: the branch a check compares with.

python3 scripts/generative/test_pr_base.py
"""

import json
import os
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pr_base


def runner(pr=None, upstream=None, branch="m1/tooling-batch-4", gh_missing=False):
    """A fake subprocess.run: gh knows `pr`'s base (or no PR), git has `upstream` as the branch's upstream."""

    def run(cmd, **kwargs):
        if cmd[0] == "gh":
            if gh_missing:
                raise FileNotFoundError("gh")
            if pr is None:
                return subprocess.CompletedProcess(cmd, 1, "", "no pull requests found")
            return subprocess.CompletedProcess(
                cmd, 0, json.dumps({"baseRefName": pr}), ""
            )
        if cmd[:2] == ["git", "rev-parse"] and cmd[-1] == "@{upstream}":
            if upstream is None:
                return subprocess.CompletedProcess(
                    cmd, 128, "", "no upstream configured"
                )
            return subprocess.CompletedProcess(cmd, 0, upstream + "\n", "")
        if cmd[:2] == ["git", "rev-parse"] and cmd[-1] == "HEAD":
            return subprocess.CompletedProcess(cmd, 0, branch + "\n", "")
        raise AssertionError(cmd)

    return run


class BaseBranch(unittest.TestCase):
    def test_the_open_prs_base_comes_first(self):
        run = runner(pr="m1-perform-techno", upstream="origin/generative")
        self.assertEqual(
            pr_base.base_branch(run=run), ("m1-perform-techno", "the PR's base")
        )

    def test_without_a_pr_the_upstream_branch_it_was_made_from(self):
        run = runner(upstream="origin/m1-perform-techno")
        self.assertEqual(
            pr_base.base_branch(run=run), ("m1-perform-techno", "the branch's upstream")
        )

    def test_an_upstream_of_the_same_name_is_the_pushed_branch_not_a_base(self):
        run = runner(upstream="origin/m1/tooling-batch-4")
        self.assertEqual(pr_base.base_branch(run=run), ("generative", "the default"))

    def test_an_upstream_on_another_remote_isnt_taken(self):
        run = runner(upstream="upstream/main")
        self.assertEqual(pr_base.base_branch(run=run), ("generative", "the default"))

    def test_no_gh_and_no_upstream_is_generative(self):
        self.assertEqual(
            pr_base.base_branch(run=runner(gh_missing=True)),
            ("generative", "the default"),
        )


def git(cwd, *args):
    return subprocess.run(
        ["git", *args], cwd=cwd, check=True, capture_output=True, text=True
    ).stdout.strip()


class Resolve(unittest.TestCase):
    """resolve() in a scratch clone of a scratch origin that has generative and m2 branches."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.origin = os.path.join(self.tmp.name, "origin")
        self.repo = os.path.join(self.tmp.name, "repo")
        os.makedirs(self.origin)
        git(self.origin, "init", "-q", "-b", "generative")
        for args in (
            ("user.email", "t@t"),
            ("user.name", "t"),
            ("commit.gpgsign", "false"),
        ):
            git(self.origin, "config", *args)
        git(self.origin, "commit", "-q", "--allow-empty", "-m", "first")
        git(self.origin, "branch", "m2")
        git(self.tmp.name, "clone", "-q", self.origin, self.repo)
        for args in (
            ("user.email", "t@t"),
            ("user.name", "t"),
            ("commit.gpgsign", "false"),
        ):
            git(self.repo, "config", *args)
        git(self.repo, "switch", "-q", "-c", "work")
        # The local m2 is stale: origin moves on after the clone.
        git(self.repo, "branch", "m2", "origin/m2")
        git(self.origin, "switch", "-q", "m2")
        git(self.origin, "commit", "-q", "--allow-empty", "-m", "on m2")
        self.origin_m2 = git(self.origin, "rev-parse", "HEAD")

    def resolve(self, base=None):
        return pr_base.resolve(base, repo=self.repo)

    def test_a_bare_name_is_origins_branch_fetched_not_the_stale_local_one(self):
        self.assertEqual(self.resolve("m2"), ("origin/m2", "given", None))
        self.assertEqual(git(self.repo, "rev-parse", "origin/m2"), self.origin_m2)

    def test_origin_slash_name_is_fetched_too(self):
        self.assertEqual(self.resolve("origin/m2"), ("origin/m2", "given", None))
        self.assertEqual(git(self.repo, "rev-parse", "origin/m2"), self.origin_m2)

    def test_without_a_base_it_is_pr_bases_branch_with_its_why(self):
        with mock.patch.object(
            pr_base, "base_branch", lambda repo, run: ("m2", "the PR's base")
        ):
            self.assertEqual(self.resolve(), ("origin/m2", "the PR's base", None))
        self.assertEqual(git(self.repo, "rev-parse", "origin/m2"), self.origin_m2)

    def test_a_commit_is_taken_as_given(self):
        head = git(self.repo, "rev-parse", "HEAD")
        self.assertEqual(self.resolve(head), (head, "given", None))
        self.assertEqual(self.resolve("HEAD~0"), ("HEAD~0", "given", None))

    def test_offline_it_uses_the_copy_fetched_last_and_says_so(self):
        git(
            self.repo,
            "remote",
            "set-url",
            "origin",
            os.path.join(self.tmp.name, "gone"),
        )
        ref, why, note = self.resolve("m2")
        self.assertEqual((ref, why), ("origin/m2", "given"))
        self.assertIn("couldn't fetch origin/m2", note)

    def test_a_branch_origin_hasnt_stops_with_a_clear_message(self):
        with self.assertRaises(pr_base.BaseError) as caught:
            self.resolve("origin/m9")
        self.assertEqual(caught.exception.ref, "origin/m9")
        self.assertIn("no origin/m9 to compare with (given)", str(caught.exception))

    def test_describe_says_the_ref_and_why(self):
        self.assertEqual(
            pr_base.describe("origin/m2", "the PR's base", None),
            "base: origin/m2 (the PR's base)",
        )
        self.assertEqual(
            pr_base.describe("origin/m2", "given", "stale"),
            "base: origin/m2 (given); stale",
        )


if __name__ == "__main__":
    unittest.main()
