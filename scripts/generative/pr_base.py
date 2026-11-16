"""The branch a check compares a branch with, for check_all.py, goldens.py and ./dbt sizediff: the base of the
branch's open PR when gh knows one; else the branch's upstream on origin, when that's another branch (a branch made
with `git switch -c NAME origin/BASE` tracks BASE until it's pushed with -u); else generative.
"""

import json
import os
import subprocess

REPO = os.path.abspath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
)
DEFAULT = "generative"


def base_branch(repo=REPO, run=subprocess.run):
    """(branch name on origin, where it came from: "the PR's base", "the branch's upstream" or "the default")."""
    try:
        proc = run(
            ["gh", "pr", "view", "--json", "baseRefName"],
            cwd=repo,
            capture_output=True,
            text=True,
            check=False,
            timeout=30,
        )
        if proc.returncode == 0:
            return json.loads(proc.stdout)["baseRefName"], "the PR's base"
    except (OSError, ValueError, KeyError, subprocess.TimeoutExpired):
        pass

    def git(*args):
        proc = run(
            ["git", *args], cwd=repo, capture_output=True, text=True, check=False
        )
        return proc.stdout.strip() if proc.returncode == 0 else ""

    upstream = git("rev-parse", "--abbrev-ref", "--symbolic-full-name", "@{upstream}")
    branch = git("rev-parse", "--abbrev-ref", "HEAD")
    if upstream.startswith("origin/") and upstream[len("origin/") :] != branch:
        return upstream[len("origin/") :], "the branch's upstream"
    return DEFAULT, "the default"
