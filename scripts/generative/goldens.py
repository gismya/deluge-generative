"""The goldens' version rule (spec §4.4, §21.2): a genre whose golden changed has a higher version than before.

    python3 scripts/generative/goldens.py check [--base REF]

Compares each golden in tests/spec/generator/goldens/ with the same file at REF (default HEAD). A golden whose
stream changed (any line but its comments and its version line) needs its genre's version above REF's: a retune
bumps `.version` in the genre's table (src/deluge/generative/core/genres/). Prints the retuned genres, for the commit
message, and exits 1 if one changed without a bump.

`./dbt test --generator --update-goldens` rewrites the goldens and then runs this against HEAD. check_all.py runs it
against the merge base with its --base branch.
"""

import argparse
import glob
import os
import re
import subprocess
import sys

REPO = os.path.abspath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
)
GOLDENS = "tests/spec/generator/goldens"
VERSION_LINE = re.compile(r"^genre (\S+) version (\d+)$")


def parse(text):
    """(genre, version, stream lines) of a golden's text: the stream is every line but comments and the version's."""
    genre, version, stream = None, None, []
    for line in text.splitlines():
        match = VERSION_LINE.match(line)
        if match and genre is None:
            genre, version = match.group(1), int(match.group(2))
        elif not line.startswith("#"):
            stream.append(line)
    return genre, version, stream


def judge(old_text, new_text):
    """("same" | "new" | "retuned" | "unbumped", genre, old version, new version) for one golden."""
    genre, new_version, new_stream = parse(new_text)
    if old_text is None:
        return "new", genre, None, new_version
    _, old_version, old_stream = parse(old_text)
    if old_stream == new_stream:
        return "same", genre, old_version, new_version
    if (
        new_version is not None
        and old_version is not None
        and new_version > old_version
    ):
        return "retuned", genre, old_version, new_version
    return "unbumped", genre, old_version, new_version


def committed(ref, path):
    """The file at `ref`, or None if it isn't there."""
    proc = subprocess.run(
        ["git", "show", f"{ref}:{path}"],
        cwd=REPO,
        capture_output=True,
        text=True,
        check=False,
    )
    return proc.stdout if proc.returncode == 0 else None


def check(ref):
    """Prints what changed against `ref`. Returns the exit code: 1 if a golden changed without a version bump."""
    failed = False
    for path in sorted(glob.glob(os.path.join(REPO, GOLDENS, "*.txt"))):
        relative = os.path.relpath(path, REPO)
        with open(path, encoding="utf-8") as f:
            verdict, genre, old, new = judge(committed(ref, relative), f.read())
        if verdict == "retuned":
            print(
                f"retuned {genre}: version {old} -> {new}. Name it in the commit message."
            )
        elif verdict == "new":
            print(f"new golden for {genre}, version {new}")
        elif verdict == "unbumped":
            failed = True
            print(
                f"{genre}'s golden changed but its version is still {new} ({ref} has {old}): if the change is meant, "
                f"bump .version in {genre}'s table (src/deluge/generative/core/genres/) and run "
                "./dbt test --generator --update-goldens again",
                file=sys.stderr,
            )
    return 1 if failed else 0


def main():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    sub = p.add_subparsers(dest="command", required=True)
    c = sub.add_parser("check", help="check the version rule against a ref")
    c.add_argument(
        "--base", default="HEAD", help="the ref to compare with (default HEAD)"
    )
    args = p.parse_args()
    return check(args.base)


if __name__ == "__main__":
    sys.exit(main())
