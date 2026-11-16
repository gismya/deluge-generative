"""Finds the firmware's ARM toolchain (arm-none-eabi-*) for the scripts that read the firmware's code or types.

On PATH inside `./dbt exec`; outside it, under the toolchain folder ./dbt uses: $DBT_TOOLCHAIN_PATH, else the main
checkout's for a linked worktree (as ./dbt finds it), else this checkout's, at toolchain/v<REQUIRED_VERSION>/<host>/.
"""

import glob
import os
import shutil
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))


def toolchain_roots(repo=REPO, environ=None):
    environ = os.environ if environ is None else environ
    roots = []
    if environ.get("DBT_TOOLCHAIN_PATH"):
        roots.append(environ["DBT_TOOLCHAIN_PATH"])
    common = subprocess.run(
        ["git", "-C", repo, "rev-parse", "--path-format=absolute", "--git-common-dir"],
        capture_output=True,
        text=True,
        check=False,
    ).stdout.strip()
    if common:
        roots.append(os.path.dirname(common))
    roots.append(repo)
    return roots


def required_version(repo=REPO):
    try:
        with open(os.path.join(repo, "toolchain", "REQUIRED_VERSION")) as fh:
            return fh.read().strip()
    except OSError:
        return "*"


def find(name, repo=REPO, environ=None):
    """The path of arm-none-eabi-NAME (g++, objdump, nm ...). Raises FileNotFoundError naming where it looked."""
    tool = f"arm-none-eabi-{name}"
    on_path = shutil.which(tool)
    if on_path:
        return on_path
    version = required_version(repo)
    looked = []
    for root in toolchain_roots(repo, environ):
        pattern = os.path.join(
            root, "toolchain", f"v{version}", "*", "arm-none-eabi-gcc", "bin", tool
        )
        looked.append(pattern)
        found = sorted(glob.glob(pattern))
        if found:
            return found[0]
    raise FileNotFoundError(
        f"no {tool} on PATH or at "
        + ", ".join(looked)
        + " (run ./dbt once to fetch it)"
    )
