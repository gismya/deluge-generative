"""The branch a check compares a branch with, for check_all.py, goldens.py and ./dbt sizediff: the base of the
branch's open PR when gh knows one; else the branch's upstream on origin, when that's another branch (a branch made
with `git switch -c NAME origin/BASE` tracks BASE until it's pushed with -u); else generative.

resolve() turns that, or a base the user gave, into the ref to compare with: origin/NAME, fetched first, so a stale
local branch or an old fetch never sets the merge base.
"""

import json
import os
import re
import subprocess
from typing import NamedTuple

REPO = os.path.abspath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
)
DEFAULT = "generative"


def base_branch(repo=REPO, run=subprocess.run):
    """(branch name on origin, where it came from: "the PR's base", "the branch's upstream" or "the default").

    When gh fails for another reason than the branch having no PR (not logged in, offline, not installed), the why
    ends "; gh failed: ...": the PR's base may not be the base used."""
    gh_error = ""
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
        if "no pull requests found" not in proc.stderr:
            gh_error = (proc.stderr.strip().splitlines() or [""])[
                0
            ] or f"exit {proc.returncode}"
    except (OSError, ValueError, KeyError, subprocess.TimeoutExpired) as e:
        gh_error = str(e) or type(e).__name__

    def git(*args):
        proc = run(
            ["git", *args], cwd=repo, capture_output=True, text=True, check=False
        )
        return proc.stdout.strip() if proc.returncode == 0 else ""

    def why(text):
        return f"{text}; gh failed: {gh_error}" if gh_error else text

    upstream = git("rev-parse", "--abbrev-ref", "--symbolic-full-name", "@{upstream}")
    branch = git("rev-parse", "--abbrev-ref", "HEAD")
    if upstream.startswith("origin/") and upstream[len("origin/") :] != branch:
        return upstream[len("origin/") :], why("the branch's upstream")
    return DEFAULT, why("the default")


class BaseError(Exception):
    """The base can't be found. `ref` is the origin/NAME that was looked for."""

    def __init__(self, message, ref):
        super().__init__(message)
        self.ref = ref


def is_revision(base):
    """Whether `base` names a commit by itself rather than a branch: HEAD, a revision expression (`^`, `~`, `@{`) or
    a hex SHA. Such a base is taken as given, never looked for on origin. A bare `@` can be part of a branch's name
    (`user@topic`)."""
    return (
        base == "HEAD"
        or any(c in base for c in "^~")
        or "@{" in base
        or re.fullmatch(r"[0-9a-f]{7,40}", base) is not None
    )


def resolve(base=None, repo=REPO, run=subprocess.run):
    """(ref, why, note) for the base a check compares with, after `git fetch origin NAME`.

    base: None for base_branch()'s; a branch name, bare or as origin/NAME, gives origin/NAME (a bare name never
    means the local branch, which may be stale); HEAD, a revision expression or a SHA that is a commit here is taken
    as given; any other name origin has no branch for (a local branch, a tag) is taken as given, with the fetch's
    error as the note. note is None, or says the fetch failed and origin/NAME is the copy fetched last, or that a
    revision counts from a local branch origin has too (generative~1: the local generative may be stale). Raises
    BaseError when there is nothing to compare with."""

    def git(*args, timeout=None):
        return run(
            ["git", *args],
            cwd=repo,
            capture_output=True,
            text=True,
            check=False,
            timeout=timeout,
        )

    def is_commit(name):
        return (
            git("rev-parse", "--verify", "--quiet", f"{name}^{{commit}}").returncode
            == 0
        )

    if base is not None and is_revision(base) and is_commit(base):
        # generative~1 counts from the local generative, which may be behind origin's.
        branch = re.split(r"[\^~]|@\{", base, maxsplit=1)[0]
        stale = branch and all(
            git("show-ref", "--verify", "--quiet", ref).returncode == 0
            for ref in (f"refs/heads/{branch}", f"refs/remotes/origin/{branch}")
        )
        note = (
            f"{base} counts from the local branch {branch}, not origin/{branch}, which may be ahead of it"
            if stale
            else None
        )
        return base, "given", note
    if base is not None and is_revision(base) and not re.fullmatch(r"[0-9a-f]+", base):
        raise BaseError(
            f"{base} isn't a commit here; pass --base with a branch on origin or a commit",
            base,
        )
    if base is None:
        branch, why = base_branch(repo, run)
    else:
        branch, why = base.removeprefix("origin/"), "given"
    ref = f"origin/{branch}"

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
    if base is not None and not base.startswith("origin/") and is_commit(base):
        note = (
            f"couldn't fetch {ref} ({error}): using {base} as given" if error else None
        )
        return base, why, note
    raise BaseError(
        f"no {ref} to compare with ({why}): "
        + (error or "origin has no such branch")
        + "; pass --base with a branch on origin or a commit",
        ref,
    )


def describe(ref, why, note):
    """The line a check prints about its base, as `./dbt sizediff` does."""
    return f"base: {ref} ({why})" + (f"; {note}" if note else "")


class Base(NamedTuple):
    """What merge_base() found. merge_base is "" when there is none; error is the BaseError's message when the base
    couldn't be resolved (ref is then the origin/NAME looked for), else None."""

    ref: str
    why: str | None
    note: str | None
    merge_base: str
    error: str | None

    def line(self):
        """The line to print about the base: describe()'s, or why there is none."""
        if self.error:
            return self.error
        line = describe(self.ref, self.why, self.note)
        return line if self.merge_base else f"{line}; no merge base with HEAD"


def merge_base(base=None, repo=REPO, run=subprocess.run):
    """resolve(base), then the merge base of HEAD with the ref, as a Base. Never raises BaseError: a base that can't
    be resolved comes back with its error and the ref looked for, so a caller can fall back (to HEAD, to skipping
    a check) or stop."""
    try:
        ref, why, note = resolve(base, repo, run)
        error = None
    except BaseError as e:
        ref, why, note, error = e.ref, None, None, str(e)
    found = run(
        ["git", "merge-base", "HEAD", ref],
        cwd=repo,
        capture_output=True,
        text=True,
        check=False,
    )
    return Base(
        ref, why, note, found.stdout.strip() if found.returncode == 0 else "", error
    )
