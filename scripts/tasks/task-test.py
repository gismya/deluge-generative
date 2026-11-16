#! /usr/bin/env python3
import argparse
import os
import subprocess
import sys

import util

BUILD_DIR = "build/tests"
# A sanitized build gets its own directory, so switching doesn't rebuild the plain one.
SANITIZE_BUILD_DIR = "build/tests-sanitize"
GENERATOR_TARGET = "generator_specs"
GENERATOR_LABEL = "generator"
GOLDEN_TEST = "golden_spec"
GOLDENS = os.path.join("tests", "spec", "generator", "goldens")


def argparser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="test", description="Run deluge tests")
    parser.group = "Development"
    parser.add_argument(
        "-n",
        "--no-run",
        action="store_true",
        help="Only build the tests, without running them.",
    )
    parser.add_argument(
        "--generator",
        action="store_true",
        help=f"Build only the Generator's specs ({GENERATOR_TARGET}) and run only them.",
    )
    parser.add_argument(
        "--sanitize",
        action="store_true",
        help="Abort the Generator specs on signed integer overflow, as CI's x86 job does "
        f"(GENERATOR_SPECS_SANITIZE=ON). Builds in {SANITIZE_BUILD_DIR}.",
    )
    parser.add_argument(
        "--update-goldens",
        action="store_true",
        help="Rewrite the Generator's goldens (tests/spec/generator/goldens/) from what the genres play now, then "
        "check that each genre whose golden changed has a higher version than at the merge base with the PR's base "
        "branch, the same base as check_all.py's (scripts/generative/goldens.py; spec 4.4, 21.2). "
        "Implies --generator.",
    )

    return parser


def cmake_build(build_dir: str, target: str | None) -> int:
    cmake_args = ["cmake"]
    cmake_args += ["--build", build_dir]
    if target:
        cmake_args += ["--target", target]

    return subprocess.run(cmake_args, env=os.environ, check=False).returncode


def cmake_test(build_dir: str, label: str | None, update_goldens: bool = False) -> int:
    cmake_args = ["ctest"]
    cmake_args += ["--test-dir", build_dir, "-C", "Debug"]
    env = dict(os.environ, CTEST_OUTPUT_ON_FAILURE="1")
    if update_goldens:
        # golden_spec writes each golden that differs instead of failing (tests/spec/generator/golden.h).
        cmake_args += ["-R", f"^{GOLDEN_TEST}$", "--verbose"]
        env["GENERATOR_UPDATE_GOLDENS"] = "1"
    elif label:
        cmake_args += ["-L", label]

    return subprocess.run(cmake_args, env=env, check=False).returncode


def check_golden_versions() -> int:
    """A golden that changed since the branch's merge base needs its genre's version bumped
    (scripts/generative/goldens.py, which finds the base as check_all.py does)."""
    script = os.path.join("scripts", "generative", "goldens.py")
    return subprocess.run([sys.executable, script, "check"], check=False).returncode


def golden_files() -> dict[str, bytes]:
    """The goldens' contents by name, to tell whether an update wrote any."""
    out = {}
    for name in sorted(os.listdir(GOLDENS)):
        with open(os.path.join(GOLDENS, name), "rb") as f:
            out[name] = f.read()
    return out


def cmake_configure(build_dir: str, sanitize: bool) -> int:
    cmake_args = ["cmake"]
    cmake_args += ["-S", "tests/"]
    cmake_args += ["-B", build_dir]
    cmake_args += ["-G", "Ninja Multi-Config"]  # generator
    if sanitize:
        cmake_args += ["-DGENERATOR_SPECS_SANITIZE=ON"]

    return subprocess.run(cmake_args, env=os.environ, check=False).returncode


def main() -> int:
    (args, _unknown_args) = argparser().parse_known_args()

    os.chdir(util.get_git_root())

    generator = args.generator or args.update_goldens
    build_dir = SANITIZE_BUILD_DIR if args.sanitize else BUILD_DIR
    target = GENERATOR_TARGET if generator else None
    label = GENERATOR_LABEL if generator else None

    # The cache, not the folder: a folder left by an interrupted or failed configure has none.
    if not os.path.exists(os.path.join(build_dir, "CMakeCache.txt")):
        result = cmake_configure(build_dir, args.sanitize)
        if result != 0:
            return result

    build = cmake_build(build_dir, target)
    if build != 0 or args.no_run:
        return build

    if args.update_goldens:
        before = golden_files()
        result = cmake_test(build_dir, label, update_goldens=True)
        # The version check whenever goldens were written, even if another spec failed: a bump the update needs
        # shows now, not on the next run.
        if result == 0 or golden_files() != before:
            versions = check_golden_versions()
            return result or versions
        return result
    return cmake_test(build_dir, label)


if __name__ == "__main__":
    sys.exit(main())
