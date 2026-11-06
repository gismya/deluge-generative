"""Build a beta and publish it as a GitHub pre-release on gismya/deluge-generative (spec §22.4).

Runs `./dbt build release`, zips the committed `pack/` tree (laid out as on the card, spec §18.1), and attaches both
to a new pre-release. Run it from the repo, on the commit to release, with that commit pushed to origin:

    python3 scripts/generative/beta_release.py --dry-run   # build and zip into build/beta-release/ only
    python3 scripts/generative/beta_release.py             # also publish

Stdlib only.
"""

import argparse
import glob
import os
import shutil
import subprocess
import sys
import zipfile

FORK = "gismya/deluge-generative"
OUT_DIR = os.path.join("build", "beta-release")
PREFIX = "beta-release: "


def log(msg):
    print(PREFIX + msg, flush=True)


def fail(msg):
    sys.exit(PREFIX + msg)


def git(*args):
    return subprocess.run(
        ["git", *args], check=True, capture_output=True, text=True
    ).stdout.strip()


def build_firmware():
    subprocess.run(["./dbt", "build", "release"], check=True)
    bins = glob.glob(os.path.join("build", "Release", "deluge*.bin"))
    if not bins:
        fail("the build made no build/Release/deluge*.bin")
    return max(bins, key=os.path.getmtime)


def zip_pack(sha, path):
    """Zip pack/ as committed at sha, with paths relative to the card root."""
    archive = subprocess.run(
        ["git", "archive", "--format=zip", "-o", path, f"{sha}:pack"],
        capture_output=True,
        text=True,
        check=False,
    )
    if archive.returncode != 0:
        log("no pack/ in this commit, so the pack zip is empty")
        with zipfile.ZipFile(path, "w"):
            pass


def publish(tag, title, sha, assets):
    # The repo is fixed and passed explicitly, so neither origin nor gh's default
    # repo can redirect the release. GH_REPO is dropped too, in case a gh version
    # ever lets it win over --repo.
    env = {k: v for k, v in os.environ.items() if k != "GH_REPO"}
    notes = (
        f"Generative beta built from {sha}.\n\n"
        "Copy the .bin to the top level of the SD card (remove any other .bin there), then hold SHIFT while "
        "powering on. Unzip the pack zip onto the card root."
    )
    subprocess.run(
        [
            "gh",
            "release",
            "create",
            tag,
            *assets,
            "--repo",
            FORK,
            "--target",
            sha,
            "--prerelease",
            "--title",
            title,
            "--notes",
            notes,
        ],
        check=True,
        env=env,
    )


def main(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--dry-run", action="store_true", help="build and zip, but publish nothing"
    )
    args = parser.parse_args(argv)

    os.chdir(git("rev-parse", "--show-toplevel"))
    sha = git("rev-parse", "HEAD")
    short_sha = sha[:9]
    tag = "generative-beta-" + short_sha
    if git("status", "--porcelain"):
        if not args.dry_run:
            fail("uncommitted changes; the release would not match the commit it tags")
        log(
            "uncommitted changes: this dry run is not what a release of this commit would contain"
        )

    # Build before making OUT_DIR: dbt configures only when build/ is missing, so
    # an early build/beta-release/ would stop a fresh checkout being configured.
    built = build_firmware()
    os.makedirs(OUT_DIR, exist_ok=True)
    bin_path = os.path.join(OUT_DIR, tag + ".bin")
    pack_path = os.path.join(OUT_DIR, tag + "-pack.zip")
    shutil.copyfile(built, bin_path)
    zip_pack(sha, pack_path)
    log(f"built {bin_path} and {pack_path}")

    if args.dry_run:
        log("dry run: nothing published")
        return 0

    publish(tag, "Generative beta " + short_sha, sha, [bin_path, pack_path])
    log(f"published {tag} on {FORK}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
