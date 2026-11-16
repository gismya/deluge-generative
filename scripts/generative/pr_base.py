"""The branch a check compares a branch with, for check_all.py, goldens.py and ./dbt sizediff: the base of the
branch's open PR when gh knows one; else the branch's upstream on origin, when that's another branch (a branch made
with `git switch -c NAME origin/BASE` tracks BASE until it's pushed with -u); else generative.

resolve() turns that, or a base the user gave, into the ref to compare with: origin/NAME, fetched first, so a stale
local branch or an old fetch never sets the merge base.
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


class BaseError(Exception):
    """The base can't be found. `ref` is the origin/NAME that was looked for."""

    def __init__(self, message, ref):
        super().__init__(message)
        self.ref = ref


def resolve(base=None, repo=REPO, run=subprocess.run):
    """(ref, why, note) for the base a check compares with, after `git fetch origin NAME`.

    base: None for base_branch()'s; a branch name, bare or as origin/NAME, gives origin/NAME (a bare name never
    means the local branch, which may be stale); anything origin has no branch for (a commit, a local branch) is
    taken as given. note is None, or says the fetch failed and origin/NAME is the copy fetched last. Raises
    BaseError when there is nothing to compare with."""
    if base is None:
        branch, why = base_branch(repo, run)
    else:
        branch, why = base.removeprefix("origin/"), "given"
    ref = f"origin/{branch}"

    def git(*args, timeout=None):
        return run(
            ["git", *args],
            cwd=repo,
            capture_output=True,
            text=True,
            check=False,
            timeout=timeout,
        )

    try:
        fetched = git("fetch", "--quiet", "origin", branch, timeout=120)
        error = ""
        if fetched.returncode:
            error = fetched.stderr.strip() or f"git fetch exited {fetched.returncode}"
    except (OSError, subprocess.TimeoutExpired) as e:
        error = str(e)
    if git("show-ref", "--verify", "--quiet", f"refs/remotes/{ref}").returncode == 0:
        note = (
            f"couldn't fetch {ref} ({error}): using the copy fetched last"
            if error
            else None
        )
        return ref, why, note
    if (
        base is not None
        and not base.startswith("origin/")
        and git("rev-parse", "--verify", "--quiet", f"{base}^{{commit}}").returncode
        == 0
    ):
        return base, why, None
    raise BaseError(
        f"no {ref} to compare with ({why}): "
        + (error or "origin has no such branch")
        + "; pass --base with a branch on origin or a commit",
        ref,
    )


def describe(ref, why, note):
    """The line a check prints about its base, as `./dbt sizediff` does."""
    return f"base: {ref} ({why})" + (f"; {note}" if note else "")
