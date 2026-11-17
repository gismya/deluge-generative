"""Tests for beta_release.py, run through its command line.

Each test runs the script in a throwaway git repo that has a fake ./dbt (it writes a .bin), fake pack checks
(scripts/generative/make_pack.py and check_pack_velocity.py: they log their run and the pack they were given, and fail
when told to) and a fake gh on PATH (it records a release's arguments, and finds a release or tag only when told to),
so no firmware is built and nothing is published.

    python3 scripts/generative/test_beta_release.py
"""

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "beta_release.py")
FORK = "gismya/deluge-generative"
UPSTREAM = "SynthstromAudible/DelugeFirmware"

# Like the real dbt, it configures only when build/ is missing, and fails if build/ exists without a configured cache.
FAKE_DBT = """#!/bin/sh
echo "$@" >> dbt.log
if [ ! -d build ]; then mkdir build && touch build/CMakeCache.txt; fi
[ -f build/CMakeCache.txt ] || { echo "Error: could not load cache" >&2; exit 1; }
mkdir -p build/Release
printf 'firmware' > build/Release/deluge.bin
"""

# A pack check: logs its name and arguments to checks.log (its last argument, the pack folder, as PACK) and the
# pack's files with their contents to packs.log, and exits 1 when FAIL_<NAME> is set.
FAKE_CHECK = """import os, sys
name = os.path.splitext(os.path.basename(__file__))[0]
pack = sys.argv[-1]
with open("checks.log", "a") as f:
    f.write(" ".join([name, *sys.argv[1:-1], "PACK"]) + "\\n")
with open("packs.log", "a") as f:
    for folder, _, files in sorted(os.walk(pack)):
        for file in sorted(files):
            path = os.path.join(folder, file)
            f.write(os.path.relpath(path, pack) + "=" + open(path).read().strip() + "\\n")
sys.exit(1 if os.environ.get("FAIL_" + name.upper()) else 0)
"""

# gh release view and gh api (the tag probe) log their call to $GH_LOG.probes and find the release or tag only when
# GH_HAS_RELEASE or GH_HAS_TAG is set; GH_DOWN fails them as if offline. Anything else (gh release create) records
# its arguments in $GH_LOG.
FAKE_GH = """#!/bin/sh
if [ "$1 $2" = "release view" ] || [ "$1" = "api" ]; then
    echo "$*" >> "$GH_LOG.probes"
    if [ -n "${GH_DOWN:-}" ]; then echo "error connecting to api.github.com" >&2; exit 1; fi
    if [ "$1" = "api" ]; then
        [ -n "${GH_HAS_TAG:-}" ] && { echo '{"ref": "x"}'; exit 0; }
        echo "gh: Not Found (HTTP 404)" >&2; exit 1
    fi
    [ -n "${GH_HAS_RELEASE:-}" ] && { echo "title: x"; exit 0; }
    echo "release not found" >&2; exit 1
fi
{ echo "GH_REPO=${GH_REPO:-}"; for a in "$@"; do echo "$a"; done; } > "$GH_LOG"
"""


def git(cwd, *args):
    return subprocess.run(
        ["git", *args], cwd=cwd, check=True, capture_output=True, text=True
    ).stdout.strip()


class BetaReleaseTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.repo = os.path.join(self.tmp, "repo")
        os.makedirs(self.repo)
        git(self.repo, "init", "-q")
        git(self.repo, "config", "user.email", "t@example.com")
        git(self.repo, "config", "user.name", "t")
        git(self.repo, "remote", "add", "origin", f"https://github.com/{UPSTREAM}.git")
        self.write("dbt", FAKE_DBT)
        os.chmod(os.path.join(self.repo, "dbt"), 0o755)
        self.write(".gitignore", "/build\ndbt.log\nchecks.log\npacks.log\n")
        self.write("scripts/generative/make_pack.py", FAKE_CHECK)
        self.write("scripts/generative/check_pack_velocity.py", FAKE_CHECK)
        self.write("pack/SYNTHS/GENERATIVE/Stab.XML", "<sound/>")
        self.write("pack/LICENSES.md", "# Licences\n")
        self.commit()

        bindir = os.path.join(self.tmp, "bin")
        os.makedirs(bindir)
        with open(os.path.join(bindir, "gh"), "w") as f:
            f.write(FAKE_GH)
        os.chmod(os.path.join(bindir, "gh"), 0o755)
        self.gh_log = os.path.join(self.tmp, "gh.log")
        self.env = dict(
            os.environ,
            PATH=bindir + os.pathsep + os.environ["PATH"],
            GH_LOG=self.gh_log,
        )

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def write(self, rel, text):
        path = os.path.join(self.repo, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            f.write(text)

    def commit(self):
        git(self.repo, "add", "-A")
        git(self.repo, "commit", "-q", "-m", "c")

    def run_script(self, *args, env=None):
        return subprocess.run(
            [sys.executable, SCRIPT, *args],
            cwd=self.repo,
            env=env or self.env,
            capture_output=True,
            text=True,
            check=False,
        )

    def out(self, *parts):
        return os.path.join(self.repo, "build", "beta-release", *parts)

    def zip_names(self):
        zips = [n for n in os.listdir(self.out()) if n.endswith("-pack.zip")]
        self.assertEqual(len(zips), 1, zips)
        with zipfile.ZipFile(self.out(zips[0])) as z:
            return sorted(n for n in z.namelist() if not n.endswith("/"))

    def gh_args(self):
        with open(self.gh_log) as f:
            return f.read().splitlines()

    def notes(self):
        args = self.gh_args()
        return args[args.index("--notes") + 1 :]

    def checks(self):
        with open(os.path.join(self.repo, "checks.log")) as f:
            return f.read().splitlines()

    def packs(self):
        with open(os.path.join(self.repo, "packs.log")) as f:
            return f.read().splitlines()

    def probes(self):
        path = self.gh_log + ".probes"
        if not os.path.exists(path):
            return []
        with open(path) as f:
            return f.read().splitlines()

    def built(self):
        return os.path.exists(os.path.join(self.repo, "dbt.log"))

    def test_dry_run_builds_and_zips_but_publishes_nothing(self):
        result = self.run_script("--dry-run")

        self.assertEqual(result.returncode, 0, result.stderr)
        with open(os.path.join(self.repo, "dbt.log")) as f:
            self.assertEqual(f.read().split(), ["build", "release"])
        bins = [n for n in os.listdir(self.out()) if n.endswith(".bin")]
        self.assertEqual(len(bins), 1, bins)
        with open(self.out(bins[0])) as f:
            self.assertEqual(f.read(), "firmware")
        self.assertEqual(
            self.zip_names(),
            ["GENERATIVE/LICENSES.md", "SYNTHS/GENERATIVE/Stab.XML"],
        )
        self.assertFalse(os.path.exists(self.gh_log))

    def test_checks_the_pack_before_building(self):
        result = self.run_script("--dry-run")

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            self.checks(),
            [
                "make_pack --check --committed --pack PACK",
                "check_pack_velocity PACK",
            ],
        )
        self.assertEqual(
            self.packs(),
            ["LICENSES.md=# Licences", "SYNTHS/GENERATIVE/Stab.XML=<sound/>"] * 2,
        )

    def test_a_dirty_dry_run_checks_the_pack_as_committed_which_it_zips(self):
        self.write("pack/SYNTHS/GENERATIVE/Stab.XML", "<sound edited/>")
        self.write("pack/SYNTHS/GENERATIVE/Untracked.XML", "<sound/>")

        result = self.run_script("--dry-run")

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            self.packs(),
            ["LICENSES.md=# Licences", "SYNTHS/GENERATIVE/Stab.XML=<sound/>"] * 2,
        )
        self.assertEqual(
            self.zip_names(), ["GENERATIVE/LICENSES.md", "SYNTHS/GENERATIVE/Stab.XML"]
        )

    def test_refuses_a_stale_pack_before_building(self):
        result = self.run_script(env=dict(self.env, FAIL_MAKE_PACK="1"))

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("make_pack.py --check", result.stderr)
        self.assertFalse(self.built())
        self.assertFalse(os.path.exists(self.gh_log))

    def test_refuses_a_preset_without_velocity_before_building(self):
        result = self.run_script(
            "--dry-run", env=dict(self.env, FAIL_CHECK_PACK_VELOCITY="1")
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("check_pack_velocity.py", result.stderr)
        self.assertFalse(self.built())

    def test_without_a_pack_the_checks_get_an_empty_folder_and_the_pack_zip_is_empty(
        self,
    ):
        git(self.repo, "rm", "-rq", "pack")
        self.commit()

        result = self.run_script("--dry-run")

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.packs(), [])
        self.assertEqual(self.zip_names(), [])

    def test_refuses_to_publish_uncommitted_changes_before_building(self):
        self.write("pack/SYNTHS/GENERATIVE/Stab.XML", "<sound edited/>")

        result = self.run_script()

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("uncommitted", result.stderr)
        self.assertFalse(os.path.exists(os.path.join(self.repo, "dbt.log")))
        self.assertFalse(os.path.exists(self.gh_log))

    def test_dry_run_allows_uncommitted_changes(self):
        self.write("pack/SYNTHS/GENERATIVE/Stab.XML", "<sound edited/>")

        result = self.run_script("--dry-run")

        self.assertEqual(result.returncode, 0, result.stderr)

    def test_publishes_a_prerelease_on_the_fork_even_when_everything_points_upstream(
        self,
    ):
        # origin is upstream (see setUp) and GH_REPO, which gh would otherwise obey, names upstream too.
        result = self.run_script(env=dict(self.env, GH_REPO=UPSTREAM))

        self.assertEqual(result.returncode, 0, result.stderr)
        args = self.gh_args()
        self.assertEqual(args[0], "GH_REPO=")
        self.assertEqual(args[1:3], ["release", "create"])
        self.assertIn("--prerelease", args)
        self.assertEqual(args[args.index("--repo") + 1], FORK)
        self.assertEqual(
            args[args.index("--target") + 1], git(self.repo, "rev-parse", "HEAD")
        )
        self.assertNotIn(UPSTREAM, " ".join(args))
        short_sha = git(self.repo, "rev-parse", "--short=9", "HEAD")
        self.assertEqual(args[3], "generative-beta-" + short_sha)
        self.assertEqual(
            args[args.index("--title") + 1], "Generative beta " + short_sha
        )
        assets = [a for a in args if a.endswith((".bin", ".zip"))]
        self.assertEqual(
            sorted(os.path.splitext(a)[1] for a in assets), [".bin", ".zip"]
        )
        self.assertTrue(
            all(os.path.isfile(os.path.join(self.repo, a)) for a in assets), assets
        )

    def test_a_name_names_the_release_its_tag_and_its_files(self):
        result = self.run_script("--name", "Beta 1")

        self.assertEqual(result.returncode, 0, result.stderr)
        args = self.gh_args()
        self.assertEqual(args[3], "generative-beta-1")
        self.assertEqual(args[args.index("--title") + 1], "Generative Beta 1")
        assets = sorted(
            os.path.basename(a) for a in args if a.endswith((".bin", ".zip"))
        )
        self.assertEqual(
            assets, ["generative-beta-1-pack.zip", "generative-beta-1.bin"]
        )

    def test_a_leading_generative_in_the_name_is_not_doubled(self):
        for name in ("generative Beta 2", "Generative-Beta 2", " generative_ Beta 2"):
            with self.subTest(name=name):
                result = self.run_script("--name", name)

                self.assertEqual(result.returncode, 0, result.stderr)
                args = self.gh_args()
                self.assertEqual(args[3], "generative-beta-2")
                self.assertEqual(args[args.index("--title") + 1], "Generative Beta 2")

    def test_a_name_that_only_starts_with_the_letters_keeps_them(self):
        result = self.run_script("--name", "Generatively Weird")

        self.assertEqual(result.returncode, 0, result.stderr)
        args = self.gh_args()
        self.assertEqual(args[3], "generative-generatively-weird")
        self.assertEqual(
            args[args.index("--title") + 1], "Generative Generatively Weird"
        )

    def test_refuses_a_name_that_is_only_generative(self):
        for name in ("Generative", "generative - "):
            with self.subTest(name=name):
                result = self.run_script("--name", name)

                self.assertNotEqual(result.returncode, 0)
                self.assertIn("no letters or digits", result.stderr)
                self.assertFalse(self.built())

    def test_checks_the_tag_is_free_on_the_fork_before_building(self):
        result = self.run_script(
            "--name", "Beta 2", env=dict(self.env, GH_REPO=UPSTREAM)
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            self.probes(),
            [
                f"release view generative-beta-2 --repo {FORK}",
                f"api repos/{FORK}/git/ref/tags/generative-beta-2",
            ],
        )

    def test_refuses_a_name_whose_release_or_tag_exists_before_building(self):
        for has in ("GH_HAS_RELEASE", "GH_HAS_TAG"):
            with self.subTest(has):
                result = self.run_script(
                    "--name", "Beta 1", env=dict(self.env, **{has: "1"})
                )

                self.assertNotEqual(result.returncode, 0)
                self.assertIn("already has", result.stderr)
                self.assertIn("generative-beta-1", result.stderr)
                self.assertFalse(self.built())
                self.assertFalse(os.path.exists(os.path.join(self.repo, "checks.log")))
                self.assertFalse(os.path.exists(self.gh_log))

    def test_refuses_when_gh_cannot_say_whether_the_tag_is_free(self):
        result = self.run_script("--name", "Beta 2", env=dict(self.env, GH_DOWN="1"))

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("couldn't check", result.stderr)
        self.assertIn("error connecting", result.stderr)
        self.assertFalse(self.built())

    def test_a_dry_run_asks_gh_nothing(self):
        result = self.run_script("--dry-run", "--name", "Beta 2")

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.probes(), [])

    def test_the_notes_say_how_to_reach_the_mode_what_is_in_and_the_pack_licence(self):
        result = self.run_script()

        self.assertEqual(result.returncode, 0, result.stderr)
        notes = "\n".join(self.notes())
        self.assertIn("SHIFT+SONG", notes)
        self.assertIn("PERFORM", notes)
        self.assertIn("GENRE", notes)
        self.assertIn("TECHNO", notes)
        for subgenre in ("ACID", "MELODIC", "DUB", "DETROIT"):
            self.assertIn(subgenre, notes)
        self.assertIn("Only the Techno subgenres", notes)
        self.assertIn("CC0", notes)


if __name__ == "__main__":
    unittest.main()
