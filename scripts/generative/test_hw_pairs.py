"""Tests for hw_pairs.py: the summary's parsing and the run's order and stop rules, with a fake session.

python3 scripts/generative/test_hw_pairs.py
"""

import argparse
import contextlib
import io
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hw_pairs

# Excerpts of M1's hardware logs (#239's base/pr pairs, #242's session).
PASSING = """\
session: ready, sample_timer 59881
session: test_port_smoke.txt: 12 expects, 0 failed
session: perform_transition.txt
  PASS line 88: expect event gen_h16 culls=0 culled=- within=20
  PASS line 90: expect event gen_h1 within=20
  event 23.0970: gen_transition start style=2 us=99
  event 23.8457: gen_h1 steps=64 mean_ns=66246 max_ns=156945
  event 23.8457: gen_h16 culls=0 voices=2 render_max_us=566
  event 51.9545: gen_transition start style=1 us=190
  event 52.1002: gen_h1 steps=32 mean_ns=69000 max_ns=245400
  event 52.1002: gen_h16 culls=2 culled=Pad voices=11 render_max_us=1486
  event 60.0000: gen_h2 ramps=on moved=yes steps=4 params=3 mean_ns=9000 max_ns=12000 render_ns=1 render_max_ns=999999
  event 61.0000: gen_transition start stopped
session: perform_transition.txt: 146 expects, 1 failed
session: summary
  test_port_smoke.txt: 12 expects, 0 failed
  perform_transition.txt: 146 expects, 1 failed
session: 2 scripts, 1 with failures
session: lock released
session exit=1
"""

CRASHED = """\
session: building /x/build-hardware/Release (log: /x/build-hardware/build.log)
libc++abi: terminating due to unexpected exception of type RtMidiError: MidiInCore::initialize: error creating \
OS-X MIDI client object (-304).
session exit=250
"""

HUNG = """\
session: ./dbt loadfw /x/build-hardware/Release/deluge.bin
session: HUNG: no answer from the test port within 90 s after loadfw /x/build-hardware/Release/deluge.bin
session: lock released
"""


def stats(text):
    return hw_pairs.Stats().add_text(text)


class Summary(unittest.TestCase):
    def test_reads_only_event_lines(self):
        s = stats(PASSING)
        self.assertEqual(s.windows, 2)
        self.assertEqual(s.culls, 2)  # the PASS line's "culls=0" isn't an event
        self.assertEqual(s.culled, ["Pad"])

    def test_h1_mean_is_weighted_by_steps_and_max_is_the_largest(self):
        s = stats(PASSING)
        self.assertAlmostEqual(s.h1_mean_us, (66246 * 64 + 69000 * 32) / 96 / 1000)
        self.assertAlmostEqual(s.h1_max_us, 245.4)

    def test_render_max_comes_from_h16_not_h2(self):
        self.assertEqual(stats(PASSING).render_max_us, 1486)

    def test_render_mean_is_the_mean_of_each_windows_slowest_render(self):
        s = stats(PASSING)
        self.assertEqual(s.render_mean_us, (566 + 1486) / 2)
        self.assertIn("render max  1486 us  mean  1026 us", s.line())
        self.assertIsNone(stats(CRASHED).render_mean_us)

    def test_transition_start_range_skips_starts_without_a_time(self):
        s = stats(PASSING)
        self.assertEqual(s.start_us, [99, 190])
        self.assertIn("start 99-190 us (2)", s.line())

    def test_failed_counts_each_script_once_not_the_summary_repeat(self):
        self.assertEqual(stats(PASSING).failed, 1)

    def test_a_log_with_no_windows_prints_dashes(self):
        s = stats(CRASHED)
        self.assertIsNone(s.h1_mean_us)
        line = s.line()
        self.assertIn("windows   0", line)
        self.assertIn("H1 mean      - us", line)
        self.assertIn("exit 250", line)

    def write(self, root, name, text):
        path = os.path.join(root, name)
        with open(path, "w") as f:
            f.write(text)
        return path

    def test_compares_base_with_pr_and_survives_an_empty_side_log(self):
        with tempfile.TemporaryDirectory() as root:
            paths = [
                self.write(root, "base1.log", PASSING),
                self.write(root, "base2.log", CRASHED),
                self.write(
                    root,
                    "pr1.log",
                    PASSING.replace("mean_ns=66246", "mean_ns=67246").replace(
                        "render_max_us=566", "render_max_us=666"
                    ),
                ),
            ]
            lines = []
            self.assertEqual(hw_pairs.summarise(paths, out=lines.append), 0)
        self.assertEqual(
            [l.split()[0] for l in lines[:3]], ["base1.log", "base2.log", "pr1.log"]
        )
        self.assertIn("exit 1,250", next(l for l in lines if l.startswith("base ")))
        self.assertIn("H1 mean +0.7 us", lines[-1])  # 1000 ns more on 64 of 96 steps
        self.assertIn(
            "render mean +50.0 us (+4.9%)", lines[-1]
        )  # (666 + 1486) / 2 vs 1026
        self.assertIn("culls +0", lines[-1])

    def test_side_render_mean_weighs_every_window_alike(self):
        a = stats(PASSING)  # windows of 566 and 1486 us
        b = stats("  event 1.0: gen_h16 culls=0 voices=1 render_max_us=400\n")
        self.assertEqual(
            hw_pairs.Stats().add(a).add(b).render_mean_us, (566 + 1486 + 400) / 3
        )

    def test_summary_prints_to_any_stdout(self):
        with tempfile.TemporaryDirectory() as root:
            path = self.write(root, "base1.log", PASSING)
            with contextlib.redirect_stdout(io.StringIO()) as got:
                self.assertEqual(hw_pairs.main(["summary", path]), 0)
        self.assertIn("render max  1486 us", got.getvalue())

    def test_no_comparison_without_both_sides(self):
        with tempfile.TemporaryDirectory() as root:
            lines = []
            hw_pairs.summarise(
                [self.write(root, "session.log", PASSING)], out=lines.append
            )
        self.assertEqual(len(lines), 1)


class Run(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        root = self.tmp.name
        self.trees = {}
        for side in hw_pairs.SIDES:
            tree = os.path.join(root, side)
            for rel in (
                hw_pairs.PROBE,
                hw_pairs.SMOKE,
                "scripts/generative/perform_x.txt",
            ):
                os.makedirs(os.path.dirname(os.path.join(tree, rel)), exist_ok=True)
                open(os.path.join(tree, rel), "w").close()
            self.trees[side] = tree
        self.out_dir = os.path.join(root, "out")

    def args(self, rounds=2, scripts=("perform_x.txt",)):
        return argparse.Namespace(
            base=self.trees["base"],
            pr=self.trees["pr"],
            rounds=rounds,
            events="gen_h1 ",
            out=self.out_dir,
            owner="t",
            scripts=list(scripts),
        )

    def run_with(self, results, **kw):
        """Runs the pairs with a fake session answering `results` in turn: (exit code, log text)."""
        calls, lines = [], []
        results = list(results)

        def session(worktree, owner, events, scripts, log):
            code, text = results.pop(0)
            calls.append(
                (
                    os.path.basename(worktree),
                    owner,
                    events,
                    scripts,
                    os.path.basename(log),
                )
            )
            with open(log, "w") as f:
                f.write(text)
            return code

        code = hw_pairs.run_pairs(
            self.args(**kw), session=session, rev=lambda w: "abc", out=lines.append
        )
        return code, calls, lines

    def test_alternates_which_side_goes_first_with_smoke_first(self):
        code, calls, _ = self.run_with([(0, PASSING)] * 6, rounds=3)
        self.assertEqual(code, 0)
        self.assertEqual(
            [(c[0], c[1], c[4]) for c in calls],
            [
                ("base", "t-base1", "base1.log"),
                ("pr", "t-pr1", "pr1.log"),
                ("pr", "t-pr2", "pr2.log"),
                ("base", "t-base2", "base2.log"),
                ("base", "t-base3", "base3.log"),
                ("pr", "t-pr3", "pr3.log"),
            ],
        )
        self.assertEqual(
            calls[0][3], [hw_pairs.SMOKE, "scripts/generative/perform_x.txt"]
        )
        self.assertEqual(calls[0][2], "gen_h1 ")

    def test_stops_on_a_hang_or_refusal(self):
        for code in (2, 3):
            with self.subTest(code=code):
                got, calls, lines = self.run_with([(0, PASSING), (code, HUNG)])
                self.assertEqual(got, 2)
                self.assertEqual(len(calls), 2)
                self.assertIn("never clear the marker", lines[-1])

    def test_stops_without_a_midi_port(self):
        got, calls, lines = self.run_with([(250, CRASHED)])
        self.assertEqual((got, len(calls)), (1, 1))
        self.assertIn("no Deluge MIDI port", lines[-1])

    def test_stops_when_the_scripts_never_ran(self):
        got, calls, _ = self.run_with([(1, "session: lock held past --wait\n")])
        self.assertEqual((got, len(calls)), (1, 1))

    def test_failed_expects_go_on_and_fail_at_the_end(self):
        got, calls, lines = self.run_with([(0, PASSING), (1, PASSING)], rounds=1)
        self.assertEqual((got, len(calls)), (1, 2))
        self.assertIn("with failed expects", lines[-1])

    def test_refuses_a_script_missing_on_one_side(self):
        os.remove(os.path.join(self.trees["base"], "scripts/generative/perform_x.txt"))
        got, calls, lines = self.run_with([], rounds=1)
        self.assertEqual((got, calls), (1, []))
        self.assertTrue(any("base:" in l and "perform_x.txt" in l for l in lines))

    def test_session_command_quotes_the_inner_command_once(self):
        cmd = hw_pairs.session_command("t-base1", "gen_h1 |gen_h16", [hw_pairs.SMOKE])
        self.assertEqual(cmd[:2], ["./dbt", "exec"])
        self.assertEqual(
            cmd[2],
            "python3 scripts/generative/deluge_probe.py session --build hardware --owner t-base1 "
            "--events 'gen_h1 |gen_h16' scripts/generative/test_port_smoke.txt",
        )


if __name__ == "__main__":
    unittest.main()
