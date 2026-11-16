"""Tests for pr_base.py: the branch a check compares with.

python3 scripts/generative/test_pr_base.py
"""

import json
import os
import subprocess
import sys
import unittest

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


if __name__ == "__main__":
    unittest.main()
