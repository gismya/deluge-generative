"""The Python unit tests of folders, each test_*.py file in its own process, side by side (#314).

    python3 scripts/generative/unit_tests.py [FOLDER...]

FOLDER defaults to scripts/generative and scripts/tasks. Each file runs as `python3 -m unittest discover -s FOLDER -p
FILE`, as one discover over the folder would run it, so a file that needs its folder on sys.path still finds it. A
failing file's output is printed whole, the rest only counted; a file with no tests passes. The last lines are the three slowest files, "Ran N tests in S s" and OK or
FAILED (the files that failed). Exits 1 on any failure. check_all.py runs it.
"""

import concurrent.futures
import glob
import os
import re
import subprocess
import sys
import time

NO_TESTS = 5  # unittest's exit code for a file with none (test_port_emu.py is a tool, not tests)
HERE = os.path.dirname(os.path.abspath(__file__))
FOLDERS = (HERE, os.path.join(HERE, "..", "tasks"))


def run_file(path):
    """(path, exit code, output, tests run, seconds) for one test file."""
    folder, name = os.path.split(path)
    start = time.monotonic()
    result = subprocess.run(
        [sys.executable, "-m", "unittest", "discover", "-s", folder, "-p", name],
        cwd=folder,
        capture_output=True,
        text=True,
        stdin=subprocess.DEVNULL,
        check=False,
    )
    output = result.stdout + result.stderr
    ran = re.findall(r"^Ran (\d+) tests?", output, re.MULTILINE)
    return (
        path,
        result.returncode,
        output,
        int(ran[-1]) if ran else 0,
        time.monotonic() - start,
    )


def main(argv):
    folders = [os.path.abspath(f) for f in (argv or FOLDERS)]
    files = sorted(
        f for folder in folders for f in glob.glob(os.path.join(folder, "test_*.py"))
    )
    start = time.monotonic()
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 2) as pool:
        results = list(pool.map(run_file, files))
    failed = []
    for path, code, output, _, _ in results:
        if code not in (0, NO_TESTS):
            failed.append(os.path.basename(path))
            print(f"==== {path} (exit {code})\n{output}")
    total = sum(r[3] for r in results)
    slowest = sorted(results, key=lambda r: -r[4])[:3]
    print(
        "slowest: "
        + ", ".join(f"{os.path.basename(r[0])} {r[4]:.1f} s" for r in slowest)
    )
    print(f"Ran {total} tests in {time.monotonic() - start:.1f} s, {len(files)} files")
    print(f"FAILED ({', '.join(failed)})" if failed else "OK")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
