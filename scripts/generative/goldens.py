"""The goldens' version rule (spec §4.4, §21.2): a genre whose golden changed has a higher version than before.

    python3 scripts/generative/goldens.py check [--base REF]

Compares each golden in tests/spec/generator/goldens/ with the same file at REF. Without --base, REF is the merge
base of HEAD with origin/BASE, BASE being the base branch of this branch's open PR when gh knows it, else generative:
the same base as check_all.py's (pr_base.py), fetched first and printed as "goldens: base: origin/BASE (why)". So
every retune on a branch counts against what the branch started from, and a second retune of the same genre before
the merge needs no second bump. If origin/BASE can't be found, it falls back to HEAD and says so. A --base that isn't
a commit exits 2.

A golden whose stream changed (any line but its comments, its format line and its version line) needs its genre's
version above REF's. A subgenre's version is the `genre.version = N;` line in its table
(src/deluge/generative/core/genres/techno.cpp, after `subgenreOf(...)`): a retune raises N by one. Prints the retuned
genres, for the commit message (nothing checks the message), and exits 1 if one changed without a bump. A version
below REF's exits 1 too, whether or not the golden changed: versions only go up.

A golden whose `format` line went up (golden.h's kFormat: a change to the dump, its seed or its length) changed for
the harness's sake, so it needs no bump. A golden without a format line is format 1: the first goldens had none, and
their dump was format 1's. One exception: the `fingerprint` line doesn't depend on the dump's format, so if it changed
while the text in its parentheses (the fingerprint's kind: kFingerprintPieces, and how stream_fingerprint.h hashes)
stayed the same, the genre's stream changed and the version still needs the bump. A change to how the fingerprint is
computed changes that text too. And a new format is refused while the genre tables (the .cpp and .h files under
src/deluge/generative/core/genres/, as in the work tree; untracked ones don't count) differ from REF: a retune the
fingerprint doesn't see (a macro default, the tempo, a scale) would pass as reformatted. Land the format bump on its
own.

A golden that REF hasn't but a commit since REF added counts from that commit, so a branch that adds a golden
and then changes it needs the bump too. A shallow clone (CI's checkout) can't tell that commit: its oldest commits
look as if they added every file. There such a golden is new, and the output says the change since it went
unchecked; check_all.py and the update command check it locally.

Also reports a golden that REF has and the work tree doesn't (exit 0: golden_spec checks that every Techno subgenre
has one, and a removed genre takes its golden with it).

`./dbt test --generator --update-goldens` rewrites the goldens and then runs this without --base. check_all.py and
CI run it with the merge base as --base.
"""

import argparse
import glob
import os
import re
import subprocess
import sys

import pr_base

REPO = os.path.abspath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
)
GOLDENS = "tests/spec/generator/goldens"
GENRE_DATA = "src/deluge/generative/core/genres"
VERSION_LINE = re.compile(r"^genre (\S+) version (\d+)$")
FORMAT_LINE = re.compile(r"^format (\d+)$")
FINGERPRINT_LINE = re.compile(r"^fingerprint .*\((.*)\)$")
FIRST_FORMAT = 1  # a golden without a format line: the first goldens' dump
RETUNE = (
    "bump its version: the `genre.version = N;` line after `subgenreOf(...)` in its table "
    "(src/deluge/generative/core/genres/techno.cpp) goes to N + 1. Then run ./dbt test --generator "
    "--update-goldens again"
)


def parse(text):
    """(format, genre, version, fingerprint line, stream lines) of a golden's text: the stream is every line but
    comments and the format and version lines (the fingerprint line is in it too). No format line is FIRST_FORMAT."""
    fmt, genre, version, fingerprint, stream = None, None, None, None, []
    for line in text.splitlines():
        version_match = VERSION_LINE.match(line)
        format_match = FORMAT_LINE.match(line)
        if version_match and genre is None:
            genre, version = version_match.group(1), int(version_match.group(2))
        elif format_match and fmt is None:
            fmt = int(format_match.group(1))
        elif not line.startswith("#"):
            if fingerprint is None and FINGERPRINT_LINE.match(line):
                fingerprint = line
            stream.append(line)
    return FIRST_FORMAT if fmt is None else fmt, genre, version, fingerprint, stream


def kind_of(fingerprint):
    """The text in a fingerprint line's parentheses: what the fingerprint is over. None without a line."""
    return FINGERPRINT_LINE.match(fingerprint).group(1) if fingerprint else None


def judge(old_text, new_text):
    """("same" | "new" | "retuned" | "reformatted" | "unbumped" | "unbumped-fingerprint" | "lowered", genre, old,
    new) for one golden: old and new are the versions, or the formats for "reformatted". "unbumped-fingerprint": the
    format went up, but the fingerprint changed while its kind didn't. "lowered": the version went down."""
    new_format, genre, new_version, new_fingerprint, new_stream = parse(new_text)
    if old_text is None:
        return "new", genre, None, new_version
    old_format, _, old_version, old_fingerprint, old_stream = parse(old_text)
    if (
        new_version is not None
        and old_version is not None
        and new_version < old_version
    ):
        return "lowered", genre, old_version, new_version
    if old_stream == new_stream:
        return "same", genre, old_version, new_version
    if (
        new_version is not None
        and old_version is not None
        and new_version > old_version
    ):
        return "retuned", genre, old_version, new_version
    if new_format <= old_format:
        return "unbumped", genre, old_version, new_version
    if old_fingerprint != new_fingerprint and kind_of(old_fingerprint) == kind_of(
        new_fingerprint
    ):
        return "unbumped-fingerprint", genre, old_version, new_version
    return "reformatted", genre, old_format, new_format


def git(*args):
    return subprocess.run(
        ["git", *args], cwd=REPO, capture_output=True, text=True, check=False
    )


def committed(ref, path):
    """The file at `ref`, or None if it isn't there."""
    proc = git("show", f"{ref}:{path}")
    return proc.stdout if proc.returncode == 0 else None


def shallow():
    """Whether this checkout is a shallow clone: its oldest commits have no parents here."""
    return git("rev-parse", "--is-shallow-repository").stdout.strip() == "true"


def reference(ref, path, is_shallow=False):
    """(text, where) to judge the golden at `path` against: the file at `ref`; or, if `ref` hasn't it, the file as
    the commit since `ref` that added it has it. (None, ref) if neither has it, or in a shallow clone, whose oldest
    commits look as if they added every file."""
    text = committed(ref, path)
    if text is not None or is_shallow:
        return text, ref
    added = git(
        "log", "--diff-filter=A", "--format=%h", f"{ref}..HEAD", "--", path
    ).stdout.split()
    if (
        added
    ):  # newest first: a golden deleted and added again counts from its latest add
        return committed(added[0], path), added[0]
    return None, ref


def default_base():
    """The merge base of HEAD with origin/<the branch check_all.py compares with>, fetched first (pr_base.py), or
    HEAD (with a note on stderr) if there is none. Prints the base and why it is the base."""
    base = pr_base.merge_base(repo=REPO)
    print(f"goldens: {base.line()}", file=sys.stderr)
    if base.merge_base:
        return base.merge_base
    print(
        f"goldens: no merge base with {base.ref} here (pass --base): checking against HEAD",
        file=sys.stderr,
    )
    return "HEAD"


def genre_data_changed(ref):
    """Whether the genre tables' committed or staged-or-not sources (.cpp and .h) in the work tree differ from
    `ref`'s. Untracked files don't count: nothing builds them until they're added."""
    return (
        git(
            "diff", "--quiet", ref, "--", f"{GENRE_DATA}/*.cpp", f"{GENRE_DATA}/*.h"
        ).returncode
        != 0
    )


def check(ref):
    """Prints what changed against `ref`. Returns the exit code: 1 if a golden changed without a version bump, a
    version went down or a new format came with changed genre tables; 2 if `ref` isn't a commit."""
    if git("rev-parse", "--verify", "--quiet", f"{ref}^{{commit}}").returncode != 0:
        print(f"goldens: {ref} isn't a commit here", file=sys.stderr)
        return 2
    failed = False
    is_shallow = shallow()
    paths = sorted(glob.glob(os.path.join(REPO, GOLDENS, "*.txt")))
    for path in paths:
        relative = os.path.relpath(path, REPO)
        old_text, where = reference(ref, relative, is_shallow)
        with open(path, encoding="utf-8") as f:
            verdict, genre, old, new = judge(old_text, f.read())
        if verdict == "retuned":
            print(
                f"retuned {genre}: version {old} -> {new}. Name it in the commit message, and copy the fingerprint "
                "steering_fingerprint_spec's failure prints into its kPinned"
            )
        elif verdict == "reformatted" and genre_data_changed(ref):
            failed = True
            print(
                f"{genre}'s golden is in a new format ({old} -> {new}), and the genre tables ({GENRE_DATA}) changed "
                f"since {ref} too: a retune the fingerprint doesn't see would pass as the new format. Land the format "
                "bump on its own",
                file=sys.stderr,
            )
        elif verdict == "reformatted":
            print(
                f"{genre}'s golden is in a new format ({old} -> {new}): no version bump needed"
            )
        elif verdict == "lowered":
            failed = True
            print(
                f"{genre}'s version went down, {old} -> {new} ({where} has {old}): versions only go up. Put it back "
                f"to {old}, or to {old + 1} for a retune",
                file=sys.stderr,
            )
        elif verdict == "new":
            print(f"new golden for {genre}, version {new}")
            if is_shallow:
                print(
                    "  (a shallow clone: a change to it since the commit that added it isn't checked here; "
                    "check_all.py checks it)"
                )
        elif verdict == "unbumped":
            failed = True
            print(
                f"{genre}'s golden changed but its version is still {new} ({where} has {old}): if the change is meant, "
                f"{RETUNE}",
                file=sys.stderr,
            )
        elif verdict == "unbumped-fingerprint":
            failed = True
            print(
                f"{genre}'s golden is in a new format, but its fingerprint changed too, and the fingerprint doesn't "
                f"depend on the format: its stream changed. Its version is still {new}. If the change is meant, "
                f"{RETUNE}. If the fingerprint's computation changed, change the text in its parentheses as well "
                "(golden.h)",
                file=sys.stderr,
            )
    present = {os.path.basename(p) for p in paths}
    listed = git("ls-tree", "--name-only", f"{ref}:{GOLDENS}").stdout.split()
    for name in sorted(listed):
        if name.endswith(".txt") and name not in present:
            print(
                f"{name[: -len('.txt')]}'s golden is gone (it is in {ref}): golden_spec checks that every Techno "
                "subgenre has one"
            )
    return 1 if failed else 0


def main():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    sub = p.add_subparsers(dest="command", required=True)
    c = sub.add_parser("check", help="check the version rule against a ref")
    c.add_argument(
        "--base",
        help="the ref to compare with (default: the merge base with origin/<the PR's base, else generative>)",
    )
    args = p.parse_args()
    return check(args.base or default_base())


if __name__ == "__main__":
    sys.exit(main())
