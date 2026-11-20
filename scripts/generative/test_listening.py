"""The listening-session toolkit (scripts/generative/listening/, #403): job files, merging, ordering, the Recipe
check, the cutter's bar math and the page."""

import array
import unittest

import theory_review
from listening import cutter, jobs, page, plan, recipe


class JobFile(unittest.TestCase):
    def test_a_sharp_key_keeps_its_sharp(self):
        (job,) = jobs.parse_jobs(
            "e1 | base | pack | 8 | --key D# --seed 1\n", genre="psytrance"
        )
        self.assertEqual(job.recipe.key, "D#")

    def test_a_comment_is_a_whole_line(self):
        text = "# name | variant | card | bars | args\n  # indented comment\n\ne1 | base | pack | 8 | --seed 1\n"
        self.assertEqual(
            [j.name for j in jobs.parse_jobs(text, genre="psytrance")], ["e1"]
        )

    def test_a_bad_flag_fails_at_load_naming_its_line(self):
        with self.assertRaisesRegex(jobs.JobError, r"jobs:2: .*--densty"):
            jobs.parse_jobs(
                "e1 | base | pack | 8 | --seed 1\ne2 | base | pack | 8 | --densty 3\n",
                genre="psytrance",
            )

    def test_a_flag_the_runner_sets_fails_at_load(self):
        with self.assertRaisesRegex(jobs.JobError, "--seconds"):
            jobs.parse_jobs("e1 | base | pack | 8 | --seconds 30\n", genre="psytrance")


class Merging(unittest.TestCase):
    def test_jobs_of_one_variant_card_recipe_and_mutes_share_the_longest_capture(self):
        text = (
            "short | base | pack | 8  | --seed 1 --mute 2\n"
            "long  | base | pack | 40 | --mute 2 --seed 1\n"
            "other_mutes | base | pack | 8 | --seed 1\n"
            "other_card | base | stock | 8 | --seed 1 --mute 2\n"
            "other_variant | s1 | pack | 8 | --seed 1 --mute 2\n"
            "other_seed | base | pack | 8 | --seed 2 --mute 2\n"
        )
        captures = jobs.merge(jobs.parse_jobs(text, genre="psytrance"))
        by_name = {c.name: sorted(c.serves) for c in captures}
        self.assertEqual(by_name["long"], ["long", "short"])
        self.assertEqual(len(captures), 5)
        self.assertEqual(next(c for c in captures if c.name == "long").bars, 40)


ORDER_JOBS = (
    "a | base | pack | 8 | --seed 1\n"
    "b | s1   | pack | 8 | --seed 1\n"
    "c | base | pack | 8 | --seed 2\n"
    "d | base | pack | 8 | --seed 1 --mute 2\n"
    "e | s1   | pack | 8 | --seed 1 --mute 2\n"
    "f | base | pack | 8 | --seed 1 --key D#\n"
)


class Ordering(unittest.TestCase):
    def test_equal_variant_and_recipe_run_back_to_back(self):
        captures = jobs.merge(jobs.parse_jobs(ORDER_JOBS, genre="psytrance"))
        names = [c.name for c in plan.order(captures)]
        self.assertEqual(sorted(names[:4]), ["a", "c", "d", "f"])  # base first
        self.assertEqual(abs(names.index("a") - names.index("d")), 1)
        self.assertEqual(sorted(names[4:]), ["b", "e"])

    def test_one_runner_links_once_per_variant_and_recipe(self):
        captures = jobs.merge(jobs.parse_jobs(ORDER_JOBS, genre="psytrance"))
        (runner,) = plan.plan(captures, runners=1, tempo_of=lambda c: 120.0).runners
        # base seed 1, base seed 1 key D#, base seed 2, s1 seed 1
        self.assertEqual(runner.links, 4)

    def test_a_variant_stays_on_one_runner(self):
        captures = jobs.merge(jobs.parse_jobs(ORDER_JOBS, genre="psytrance"))
        p = plan.plan(captures, runners=2, tempo_of=lambda c: 120.0)
        self.assertEqual(
            sorted({c.variant for c in r.captures} for r in p.runners),
            [{"base"}, {"s1"}],
        )
        self.assertEqual(sum(r.links for r in p.runners), 4)

    def test_the_dry_run_prints_links_per_runner_and_the_seconds_captured(self):
        captures = jobs.merge(jobs.parse_jobs(ORDER_JOBS, genre="psytrance"))
        text = plan.plan(captures, runners=2, tempo_of=lambda c: 120.0).text()
        # base: 4 captures of 17 s, 3 links; s1: 2 captures, 1 link. 6 captures of 8 bars at 120 BPM: 6 x 17 s.
        self.assertRegex(text, r"runner 1: 4 captures, 3 links, 68.0 s of audio")
        self.assertRegex(text, r"runner 2: 2 captures, 1 link, 34.0 s of audio")
        self.assertIn("total: 6 captures, 4 links, 102.0 s captured", text)

    def test_seconds_come_from_bars_at_the_harness_tempo_plus_a_second(self):
        # 8 bars at 120 BPM: 16 s, and a second to spare
        self.assertEqual(plan.seconds(8, 120.0), 17.0)
        # 3 bars at 143 BPM: 5.0349 s, rounded up to a tenth, plus a second
        self.assertEqual(plan.seconds(3, 143.0), 6.1)


def theory(key_root="D#", scale="minor", **fields):
    """A harness render's THEORY: its `recipe` line's fields and its first `key` line."""
    line = {
        "genre": "psytrance",
        "seed": "1",
        "entropy": "0",
        "tempo": "143.00",
        "swing": "0",
        "energy": "25.0",
        "density": "25.0",
        "complexity": "25.0",
        "motion": "25.0",
    }
    line.update(fields)
    key = theory_review.Key(0, theory_review.PITCH_NAMES.index(key_root), scale, [])
    return theory_review.Piece(line, [], [], 1, [], [key], [], [])


class RecipeCheck(unittest.TestCase):
    def job(self, args):
        (j,) = jobs.parse_jobs(f"e1 | base | pack | 8 | {args}\n", genre="psytrance")
        return j

    def test_a_render_of_what_the_job_asked_for_passes(self):
        job = self.job("--seed 1 --key D# --scale minor --tempo 143 --density 25")
        self.assertEqual(recipe.mismatches(job, theory()), [])

    def test_a_job_whose_theory_key_differs_fails(self):
        job = self.job("--seed 1 --key D#")
        self.assertEqual(
            recipe.mismatches(job, theory(key_root="D")),
            ["key: asked D#, the harness plays D"],
        )

    def test_a_flat_spelling_matches_its_sharp(self):
        self.assertEqual(recipe.mismatches(self.job("--key Eb"), theory()), [])

    def test_seed_entropy_tempo_scale_and_macros_are_checked(self):
        job = self.job(
            "--seed 2 --entropy 5 --tempo 138.5 --scale phrygian --complexity 40"
        )
        self.assertEqual(
            recipe.mismatches(job, theory()),
            [
                "seed: asked 2, the harness plays 1",
                "entropy: asked 5, the harness plays 0",
                "complexity: asked 40, the harness plays 25.0",
                "scale: asked phrygian, the harness plays minor",
                "tempo: asked 138.5, the harness plays 143.00",
            ],
        )

    def test_what_the_job_leaves_to_the_roll_is_not_checked(self):
        self.assertEqual(
            recipe.mismatches(
                self.job("--seed 1"),
                theory(key_root="A", scale="dorian", tempo="171.00"),
            ),
            [],
        )


class Cutter(unittest.TestCase):
    def test_bars_at_a_tempo_whose_bar_is_not_a_whole_number_of_frames(self):
        # 143 BPM: a bar is 240/143 s = 74013.986 frames at 44.1 kHz. Bar 20 starts at frame 1480279.72, bar 36 at
        # 2664503.49: each rounds to its nearest frame, so clips cut back to back tile with no gap.
        self.assertEqual(cutter.window(143.0, 20, 36, 44100), (1480280, 1184223))
        self.assertEqual(
            sum(cutter.window(143.0, 20, 28, 44100)),
            cutter.window(143.0, 28, 36, 44100)[0],
        )

    def test_a_window_past_the_capture_stops_at_its_end(self):
        self.assertEqual(
            cutter.window(120.0, 2, 10, 44100, total=200000), (176400, 23600)
        )

    def test_32_bit_samples_become_16_bit_with_10_ms_fades(self):
        rate, n = 1000, 100  # 10 ms is 10 frames at 1 kHz
        frames = array.array("i", [0x40000000] * (2 * n)).tobytes()
        out = array.array("h", cutter.to_16_bit(frames, 4, 2, rate))
        self.assertEqual(len(out), 2 * n)
        self.assertEqual(out[0], 0)  # the fade's first frame
        self.assertEqual(out[2 * 50], 0x4000)  # the middle, level kept
        self.assertEqual(out[-1], 0)  # the fade out's last frame
        self.assertLess(out[2 * 5], 0x4000)


def content(**card):
    c = {
        "id": "S1",
        "ticket": "#133",
        "title": "The Arp",
        "section": "question",
        "now": "It walks the chord.",
        "options": [{"key": "A", "label": "A sequence", "variant": "s1"}],
        "listen": "The Arp, from 0:04.",
        "setup": "entropy 5",
        "clips": [{"stem": "s1-a", "src": "s1_e5", "bars": [20, 36], "label": "A"}],
    }
    c.update(card)
    return {
        "genre": "psytrance",
        "round": 3,
        "folder": "~/notes/tuning/psytrance/round-3/",
        "title": "Psytrance, round 3",
        "intro": ["Two cards."],
        "sections": [{"id": "question", "title": "Questions"}],
        "cards": [c],
    }


class Page(unittest.TestCase):
    def test_a_valid_content_file_has_no_problems(self):
        self.assertEqual(page.problems(content(), sources={"s1_e5"}), [])

    def test_a_clip_from_an_unknown_source_or_a_missing_field_is_a_problem(self):
        bad = content(
            listen="",
            clips=[{"stem": "x", "src": "nope", "bars": [0, 8], "label": "A"}],
        )
        self.assertEqual(
            page.problems(bad, sources={"s1_e5"}),
            ["S1: no listen", "S1 clip x: no capture or render named nope"],
        )

    def test_the_export_opens_with_the_skills_line(self):
        html = page.html(page.page_data(content(), clips={}))
        self.assertIn(
            "Listening-session answers (psytrance, round 3, ~/notes/tuning/psytrance/round-3/): handle them with the "
            "listening-session skill.",
            html,
        )
        self.assertIn("execCommand('copy')", html)
        self.assertNotIn("/*DATA*/null", html)


if __name__ == "__main__":
    unittest.main()
