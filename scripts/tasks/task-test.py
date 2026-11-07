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

    return parser


def cmake_build(build_dir: str, target: str | None) -> int:
    cmake_args = ["cmake"]
    cmake_args += ["--build", build_dir]
    if target:
        cmake_args += ["--target", target]

    return subprocess.run(cmake_args, env=os.environ, check=False).returncode


def cmake_test(build_dir: str, label: str | None) -> int:
    cmake_args = ["ctest"]
    cmake_args += ["--test-dir", build_dir, "-C", "Debug"]
    if label:
        cmake_args += ["-L", label]

    return subprocess.run(
        cmake_args, env=dict(os.environ, CTEST_OUTPUT_ON_FAILURE="1"), check=False
    ).returncode


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

    build_dir = SANITIZE_BUILD_DIR if args.sanitize else BUILD_DIR
    target = GENERATOR_TARGET if args.generator else None
    label = GENERATOR_LABEL if args.generator else None

    # The cache, not the folder: a folder left by an interrupted or failed configure has none.
    if not os.path.exists(os.path.join(build_dir, "CMakeCache.txt")):
        result = cmake_configure(build_dir, args.sanitize)
        if result != 0:
            return result

    build = cmake_build(build_dir, target)
    if build != 0 or args.no_run:
        return build

    return cmake_test(build_dir, label)


if __name__ == "__main__":
    sys.exit(main())
