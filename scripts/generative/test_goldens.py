"""Unit tests for goldens.py: python3 -m unittest discover -s scripts/generative -p 'test_*.py'"""

import contextlib
import io
import os
import subprocess
import tempfile
import unittest
from unittest import mock

import goldens


def golden(version, *events, comment="# made by the harness", fmt=1):
    return (
        "\n".join(
            [
                comment,
                f"format {fmt}",
                f"genre techno-acid version {version}",
                "parts 0 DRUMS",
                "",
                *events,
            ]
        )
        + "\n"
    )


class Judge(unittest.TestCase):
    def test_same_stream_is_same_whatever_the_comments(self):
        old = golden(1, "0 DRUMS Kick on v100")
        new = golden(1, "0 DRUMS Kick on v100", comment="# another comment")
        self.assertEqual(goldens.judge(old, new), ("same", "techno-acid", 1, 1))

    def test_a_changed_stream_with_a_bump_is_a_retune(self):
        old = golden(1, "0 DRUMS Kick on v100")
        new = golden(2, "0 DRUMS Kick on v90")
        self.assertEqual(goldens.judge(old, new), ("retuned", "techno-acid", 1, 2))

    def test_a_changed_stream_without_a_bump_is_refused(self):
        old = golden(3, "0 DRUMS Kick on v100")
        self.assertEqual(
            goldens.judge(old, golden(3, "0 DRUMS Kick on v90")),
            ("unbumped", "techno-acid", 3, 3),
        )
        self.assertEqual(
            goldens.judge(old, golden(2, "0 DRUMS Kick on v90")),
            ("unbumped", "techno-acid", 3, 2),
        )

    def test_a_bump_alone_is_same(self):
        old = golden(1, "0 DRUMS Kick on v100")
        self.assertEqual(
            goldens.judge(old, golden(2, "0 DRUMS Kick on v100"))[0], "same"
        )

    def test_a_new_format_needs_no_bump(self):
        old = golden(1, "0 DRUMS Kick on v100")
        new = golden(1, "0  1.1.1  DRUMS Kick on v100", fmt=2)
        self.assertEqual(goldens.judge(old, new), ("reformatted", "techno-acid", 1, 2))

    def test_a_golden_with_none_before_is_new(self):
        self.assertEqual(
            goldens.judge(None, golden(1)), ("new", "techno-acid", None, 1)
        )


class Check(unittest.TestCase):
    """check() against a scratch git repo with goldens in it."""

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        root = self.dir.name
        self.folder = os.path.join(root, goldens.GOLDENS)
        os.makedirs(self.folder)
        for args in (
            ["init", "-q"],
            ["config", "user.email", "t@t"],
            ["config", "user.name", "t"],
            ["config", "commit.gpgsign", "false"],
        ):
            subprocess.run(["git", *args], cwd=root, check=True)
        self.write("techno-acid", golden(1, "0 DRUMS Kick on v100"))
        subprocess.run(["git", "add", "-A"], cwd=root, check=True)
        subprocess.run(["git", "commit", "-qm", "goldens"], cwd=root, check=True)
        patcher = mock.patch.object(goldens, "REPO", root)
        patcher.start()
        self.addCleanup(patcher.stop)

    def write(self, name, text):
        with open(os.path.join(self.folder, name + ".txt"), "w", encoding="utf-8") as f:
            f.write(text)

    def run_check(self):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = goldens.check("HEAD")
        return code, out.getvalue(), err.getvalue()

    def test_unchanged_passes_quietly(self):
        self.assertEqual(self.run_check(), (0, "", ""))

    def test_a_retune_names_the_genre(self):
        self.write("techno-acid", golden(2, "0 DRUMS Kick on v90"))
        code, out, _ = self.run_check()
        self.assertEqual(code, 0)
        self.assertIn("retuned techno-acid: version 1 -> 2", out)

    def test_a_change_without_a_bump_fails_and_says_what_to_do(self):
        self.write("techno-acid", golden(1, "0 DRUMS Kick on v90"))
        code, _, err = self.run_check()
        self.assertEqual(code, 1)
        self.assertIn("techno-acid's golden changed but its version is still 1", err)
        self.assertIn("./dbt test --generator --update-goldens", err)

    def test_a_new_format_passes_and_says_so(self):
        self.write("techno-acid", golden(1, "0 1.1.1 DRUMS Kick on v100", fmt=2))
        code, out, _ = self.run_check()
        self.assertEqual(code, 0)
        self.assertIn("techno-acid's golden is in a new format (1 -> 2)", out)

    def test_a_new_golden_passes(self):
        self.write("techno-dub", golden(1).replace("techno-acid", "techno-dub"))
        code, out, _ = self.run_check()
        self.assertEqual(code, 0)
        self.assertIn("new golden for techno-dub, version 1", out)


if __name__ == "__main__":
    unittest.main()
