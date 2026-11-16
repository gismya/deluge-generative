#! /usr/bin/env python3
"""Compare the Release build at HEAD (working tree included) with the build at its merge base.

The base is built in its own git worktree under .cache/sizediff/base, so the working tree is
never checked out or touched, and uncommitted changes are fine. The base build uses the default
CMake options; if the current build was configured with other options, the diff says so. To check
that a change leaves the default build unchanged, configure without options first.
"""

import argparse
import importlib
import os
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

import util

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "generative"))
import arm_tools
import pr_base

CACHE = Path(".cache/sizediff")
BASE_TREE = CACHE / "base"
BASE_COMMIT = CACHE / "base-commit"
ELF = Path("build/Release/deluge.elf")


def argparser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="sizediff",
        description="Size and symbol differences between the Release build and its merge base",
    )
    parser.group = "Development"
    parser.add_argument(
        "--base",
        help="branch or commit to compare against, via its merge base with HEAD. A branch name means origin's, "
        "fetched first: --base generative compares with origin/generative, never a stale local generative "
        "(default: the branch's open PR's base, else its upstream when it tracks another branch, else generative; "
        "as check_all.py)",
    )
    parser.add_argument(
        "--all-symbols",
        action="store_true",
        help="list every changed symbol instead of the first 40",
    )
    return parser


def merge_base(base: str | None, repo: str) -> tuple[str, str, str]:
    """(merge base, the ref it is with, why that ref) for --base (pr_base.resolve: a branch is origin's, fetched
    first). Raises pr_base.BaseError with a message to show when there's nothing to compare with."""
    ref, why, note = pr_base.resolve(base, repo=repo)
    if note:
        print(f"Note: {note}")
    found = subprocess.run(
        ["git", "merge-base", ref, "HEAD"],
        cwd=repo,
        capture_output=True,
        text=True,
        check=False,
    )
    if found.returncode != 0:
        raise pr_base.BaseError(f"{ref} and HEAD have no merge base ({why})", ref)
    return found.stdout.strip(), ref, why


def git(*args: str) -> str:
    return subprocess.run(
        ["git", *args], stdout=subprocess.PIPE, check=True, text=True
    ).stdout.strip()


def build_base_tree() -> None:
    # The base tree's own dbt may predate worktree support: DBT_NO_SYNC skips its .git check, and
    # DBT_TOOLCHAIN_PATH (exported by this checkout's dbt) points it at the shared toolchain.
    env = dict(os.environ, DBT_NO_SYNC="1")
    subprocess.run(["./dbt", "build", "release"], cwd=BASE_TREE, env=env, check=True)


def build_base(commit: str) -> None:
    if not (BASE_TREE / ".git").exists():
        git("worktree", "add", "--detach", str(BASE_TREE), commit)
    elif BASE_COMMIT.exists() and BASE_COMMIT.read_text().strip() == commit:
        return
    else:
        git("-C", str(BASE_TREE), "checkout", "--detach", commit)
    build_base_tree()
    BASE_COMMIT.write_text(commit)


def sections(elf: Path) -> dict[str, int]:
    """text, data and bss sizes, from arm-none-eabi-size."""
    names, values = util.run_get_output(["arm-none-eabi-size", str(elf)]).split("\n")[
        :2
    ]
    return {
        n: int(v)
        for n, v in zip(names.split(), values.split())
        if n in ("text", "data", "bss")
    }


def symbols(elf: Path) -> dict[tuple[str, str], list[int]]:
    """(type, demangled name) -> sorted sizes. Static symbols can share a name, hence the list."""
    out = util.run_get_output(["arm-none-eabi-nm", "-S", "-C", str(elf)])
    table = defaultdict(list)
    for symbol in arm_tools.parse_nm(out):
        if symbol.size is not None:
            table[(symbol.kind, symbol.name)].append(symbol.size)
    return {key: sorted(sizes) for key, sizes in table.items()}


def cmake_options(tree: Path) -> dict[str, str]:
    cache = tree / "build" / "CMakeCache.txt"
    options = {}
    for line in cache.read_text().split("\n") if cache.exists() else []:
        if line.startswith("ENABLE_") and "=" in line:
            key, value = line.split("=", 1)
            options[key.split(":")[0]] = value
    return options


def main() -> int:
    (args, _unknown_args) = argparser().parse_known_args()

    os.chdir(util.get_git_root())
    CACHE.mkdir(parents=True, exist_ok=True)

    try:
        base, base_ref, why = merge_base(args.base, os.getcwd())
    except pr_base.BaseError as e:
        print(f"sizediff: {e}")
        return 1
    result = importlib.import_module("task-build").main(["release"])
    if result:
        return result
    build_base(base)

    for key, value in sorted(cmake_options(Path(".")).items()):
        base_value = cmake_options(BASE_TREE).get(key)
        if value != base_value:
            print(f"Note: {key}={value} here, {base_value} in the base build")

    old_sections, new_sections = sections(BASE_TREE / ELF), sections(ELF)
    print(f"Base: {base[:9]} (merge base with {base_ref}, {why})")
    for name in ("text", "data", "bss"):
        delta = new_sections[name] - old_sections[name]
        print(f"  {name:5} {new_sections[name]:>9}  {delta:+}")

    old_symbols, new_symbols = symbols(BASE_TREE / ELF), symbols(ELF)
    changed = [
        (
            kind,
            name,
            old_symbols.get((kind, name), []),
            new_symbols.get((kind, name), []),
        )
        for (kind, name) in sorted(
            old_symbols.keys() | new_symbols.keys(), key=lambda k: k[1]
        )
        if old_symbols.get((kind, name)) != new_symbols.get((kind, name))
    ]
    if not changed:
        print("Symbols: no symbol added, removed or resized")
        return 0
    print(
        f"Symbols: {len(changed)} added, removed or resized (sizes in bytes, base -> HEAD)"
    )
    shown = changed if args.all_symbols else changed[:40]
    for kind, name, old, new in shown:
        print(f"  {kind} {name}: {old or '-'} -> {new or '-'}")
    if len(shown) < len(changed):
        print(f"  ... {len(changed) - len(shown)} more, see --all-symbols")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
