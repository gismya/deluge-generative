"""The listening-session toolkit (scripts/generative/listening/, #403): job files, merging, ordering, the Recipe
check, the cutter's bar math and the page."""

import array
import contextlib
import io
import os
import subprocess
import tempfile
import types
import unittest
from unittest import mock

import theory_review
from listening import cli, cutter, jobs, page, plan, recipe, runner, variants
from test_fat_image import make_fat32


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

    def test_an_abbreviated_flag_fails_at_load(self):
        # argparse would take --sec as --seconds and --rend as --render, and the runner would drop both
        for args in ("--sec 30", "--rend /x", "--dens 30"):
            with self.subTest(args), self.assertRaises(jobs.JobError):
                jobs.parse_jobs(f"e1 | base | pack | 8 | {args}\n", genre="psytrance")

    def test_an_unknown_scale_or_genre_fails_at_load(self):
        with self.assertRaisesRegex(jobs.JobError, "--scale minr"):
            jobs.parse_jobs("e1 | base | pack | 8 | --scale minr\n", genre="psytrance")
        with self.assertRaisesRegex(jobs.JobError, "--genre psytranse"):
            jobs.parse_jobs("e1 | base | pack | 8 | --seed 1\n", genre="psytranse")

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

    def test_one_runner_links_once_per_variant_not_per_recipe(self):
        captures = jobs.merge(jobs.parse_jobs(ORDER_JOBS, genre="psytrance"))
        (runner,) = plan.plan(captures, runners=1, tempo_of=lambda c: 120.0).runners
        # base (three Recipes: a Recipe is patched into the ELF, #401), then s1
        self.assertEqual(runner.links, 2)

    def test_a_variant_stays_on_one_runner(self):
        captures = jobs.merge(jobs.parse_jobs(ORDER_JOBS, genre="psytrance"))
        p = plan.plan(captures, runners=2, tempo_of=lambda c: 120.0)
        self.assertEqual(
            sorted({c.variant for c in r.captures} for r in p.runners),
            [{"base"}, {"s1"}],
        )
        self.assertEqual(sum(r.links for r in p.runners), 2)

    def test_the_dry_run_prints_links_per_runner_and_the_seconds_captured(self):
        captures = jobs.merge(jobs.parse_jobs(ORDER_JOBS, genre="psytrance"))
        text = plan.plan(captures, runners=2, tempo_of=lambda c: 120.0).text()
        # base: 4 captures of 17 s, 1 link; s1: 2 captures, 1 link. 6 captures of 8 bars at 120 BPM: 6 x 17 s.
        self.assertRegex(text, r"runner 1: 4 captures, 1 link, 68.0 s of audio")
        self.assertRegex(text, r"runner 2: 2 captures, 1 link, 34.0 s of audio")
        self.assertIn("total: 6 captures, 2 links, 102.0 s captured", text)

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


class Recut(unittest.TestCase):
    def test_a_clip_is_cut_again_when_its_source_is_captured_again_or_its_bars_change(
        self,
    ):
        with tempfile.TemporaryDirectory() as scratch:
            os.makedirs(os.path.join(scratch, "raw"))
            wav = os.path.join(scratch, "raw", "e1.wav")

            def capture(data):
                with open(wav, "wb") as fh:
                    fh.write(data)
                with open(os.path.join(scratch, "raw", "e1.json"), "w") as fh:
                    fh.write('{"tempo": 143.0, "seconds": 41.3}')

            capture(b"one")
            clip = {"stem": "c", "src": "e1", "bars": [0, 8]}
            first = page.cut_key(scratch, clip)
            self.assertEqual(page.cut_key(scratch, clip), first)
            self.assertNotEqual(page.cut_key(scratch, dict(clip, bars=[0, 12])), first)
            os.remove(
                wav
            )  # a new capture is a new file (os.replace), so a new inode and mtime
            capture(b"two, longer")
            self.assertNotEqual(page.cut_key(scratch, clip), first)


STAMP = {
    "variant": "base",
    "diff": "-",
    "card": "pack 1 2",
    "args": "--genre psytrance --key D#",
    "head": "abc",
}


class Reuse(unittest.TestCase):
    STAMP = types.MappingProxyType(STAMP)

    def test_a_wav_captured_with_these_arguments_and_long_enough_is_reused(self):
        info = dict(self.STAMP, seconds=41.3, tempo=143.0)
        self.assertTrue(runner.is_current(info, self.STAMP, 30.0))

    def test_a_wav_captured_with_other_arguments_is_captured_again(self):
        info = dict(self.STAMP, args="--genre psytrance --key D", seconds=41.3)
        self.assertFalse(runner.is_current(info, self.STAMP, 30.0))

    def test_a_wav_from_other_source_or_another_diff_is_captured_again(self):
        self.assertFalse(
            runner.is_current(
                dict(self.STAMP, head="old", seconds=41.3), self.STAMP, 30.0
            )
        )
        self.assertFalse(
            runner.is_current(
                dict(self.STAMP, diff="old", seconds=41.3), self.STAMP, 30.0
            )
        )

    def test_a_wav_shorter_than_the_jobs_now_need_is_captured_again(self):
        self.assertFalse(
            runner.is_current(dict(self.STAMP, seconds=20.0), self.STAMP, 30.0)
        )

    def test_no_sidecar_means_capture_again(self):
        self.assertFalse(runner.is_current(None, self.STAMP, 30.0))


def git_repo(folder):
    """A repo with src/a.txt committed."""
    run = lambda *a: subprocess.run(
        ["git", "-C", folder, *a], check=True, capture_output=True
    )
    run("init", "-q")
    os.makedirs(os.path.join(folder, "src"))
    with open(os.path.join(folder, "src", "a.txt"), "w") as fh:
        fh.write("one\n")
    run("add", ".")
    run("-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "-m", "a")


class Variants(unittest.TestCase):
    def test_make_refuses_a_worktree_with_uncommitted_work_under_src_and_leaves_it(
        self,
    ):
        with (
            tempfile.TemporaryDirectory() as repo,
            tempfile.TemporaryDirectory() as session,
        ):
            git_repo(repo)
            path = os.path.join(repo, "src", "a.txt")
            with open(path, "w") as fh:
                fh.write("work in progress\n")
            edit = variants.Variant(edits=[("src/a.txt", "work", "play")])
            with self.assertRaisesRegex(ValueError, "uncommitted"):
                variants.make(repo, session, {"v": edit})
            with open(path) as fh:
                self.assertEqual(fh.read(), "work in progress\n")

    def test_make_writes_the_diff_and_leaves_a_clean_worktree_clean(self):
        with (
            tempfile.TemporaryDirectory() as repo,
            tempfile.TemporaryDirectory() as session,
        ):
            git_repo(repo)
            variants.make(
                repo,
                session,
                {"v": variants.Variant(edits=[("src/a.txt", "one", "two")])},
            )
            with open(variants.diff_path(session, "v")) as fh:
                self.assertIn("+two", fh.read())
            with open(os.path.join(repo, "src", "a.txt")) as fh:
                self.assertEqual(fh.read(), "one\n")


class PaletteCard(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = tmp.name

    def palette(self, name, data=b"<palette/>"):
        path = os.path.join(self.tmp, name)
        with open(path, "wb") as fh:
            fh.write(data)
        return path

    def test_the_genres_oled_names_are_read_from_their_tables(self):
        self.assertTrue({"AMBIENT", "ACID TECHNO", "PSYTRANCE"} <= cli.oled_names())

    def test_an_override_must_be_named_after_a_genre(self):
        names = {"AMBIENT"}
        self.assertEqual(
            cli.palette_dest(self.palette("AMBIENT.XML"), names),
            "GENERATIVE/PALETTES/AMBIENT.XML",
        )
        for bad in ("ambient.XML", "AMBIENT.xml", "AMBIENT TECHNO.XML"):
            with self.assertRaisesRegex(ValueError, "AMBIENT.XML"):
                cli.palette_dest(self.palette(bad), names)

    def test_an_override_past_the_firmwares_limit_is_refused(self):
        big = self.palette("AMBIENT.XML", b" " * (cli.PALETTE_MAX_BYTES + 1))
        with self.assertRaisesRegex(ValueError, "ignore"):
            cli.palette_dest(big, {"AMBIENT"})

    def test_card_writes_the_override_onto_its_copy_of_the_stock_card(self):
        import fat_image

        stock = os.path.join(self.tmp, "stock.img")
        make_fat32(stock)
        palette = self.palette("AMBIENT.XML", b"<palette>drone</palette>")
        scratch = os.path.join(self.tmp, "scratch")
        with contextlib.redirect_stdout(io.StringIO()):
            cli.main(
                [
                    "card",
                    "--scratch",
                    scratch,
                    "--stock",
                    stock,
                    "--no-pack",
                    "--palette",
                    palette,
                    "d1",
                ]
            )
        with fat_image.FatImage(os.path.join(scratch, "card_d1.img")) as card:
            self.assertEqual(
                card.read_file("GENERATIVE/PALETTES/AMBIENT.XML"),
                b"<palette>drone</palette>",
            )
        with fat_image.FatImage(stock) as card:
            self.assertEqual(card.listdir(), [])


class QuietBars(unittest.TestCase):
    """#422: an FX-alone job names the bars where silence is expected, for gen_emu's check."""

    def test_a_job_names_its_quiet_bars_and_still_shares_a_capture(self):
        text = (
            "fx   | base | pack | 54 | --seed 24 --mute 1 --quiet-bars 47\n"
            "fx_s | base | pack | 20 | --seed 24 --mute 1 --quiet-bars 3-4\n"
        )
        js = jobs.parse_jobs(text, genre="dub")
        self.assertEqual(js[0].quiet, (47,))
        (capture,) = jobs.merge(js)
        self.assertEqual(capture.quiet, (3, 4, 47))
        # Not part of the audio: a WAV captured before the job named them stays current
        self.assertEqual(
            capture.job.capture_flags(),
            ["--genre", "dub", "--seed", "24", "--entropy", "0", "--mute", "1"],
        )

    def test_the_runner_passes_them_to_the_capture(self):
        js = jobs.parse_jobs(
            "fx | base | pack | 8 | --mute 1 --quiet-bars 3,5", genre="dub"
        )
        r = runner.Run("dub", "/s", "/x", js, 1)
        cmd = r.command(1, "/wt", r.captures[0], "/out.wav", 17.0)
        self.assertEqual(cmd[cmd.index("--quiet-bars") + 1], "3,5")
        plain = runner.Run(
            "dub", "/s", "/x", jobs.parse_jobs("a | base | pack | 8 |", "dub"), 1
        )
        self.assertNotIn(
            "--quiet-bars", plain.command(1, "/wt", plain.captures[0], "/o.wav", 17.0)
        )


class Retry(unittest.TestCase):
    """#422: a capture whose check fails twice with the same bytes is the audio itself: kept, with the failure noted."""

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.scratch = tmp.name
        js = jobs.parse_jobs("fx | base | pack | 8 | --mute 1", genre="dub")
        self.run_ = runner.Run("dub", self.scratch, self.scratch, js, 1)
        self.run_.tempos = {"fx": 120.0}
        self.run_.head = "abc"
        os.makedirs(self.run_.raw)
        self.attempt = 0

    def capture_with(self, outputs):
        def fake(cmd, **kw):
            out = cmd[cmd.index("--out") + 1]
            with open(out, "wb") as fh:
                fh.write(outputs[self.attempt])
            self.attempt += 1
            kw["stdout"].write("FAIL silent in 1 bar(s)\n")
            return subprocess.CompletedProcess(cmd, 1)

        with (
            mock.patch.object(runner.subprocess, "run", fake),
            mock.patch.object(runner, "RETRY_WAIT_S", 0),
            mock.patch.object(runner.Run, "wait_turn", lambda self: None),
            contextlib.redirect_stdout(io.StringIO()) as said,
        ):
            ok = self.run_.capture(1, "/wt", self.run_.captures[0])
        return ok, said.getvalue()

    def test_a_byte_identical_retry_is_kept_with_its_failure_noted(self):
        ok, said = self.capture_with([b"same", b"same"])
        self.assertTrue(ok)
        self.assertIn("byte-identical", said)
        with open(os.path.join(self.run_.raw, "fx.wav"), "rb") as fh:
            self.assertEqual(fh.read(), b"same")
        info = runner.read_info(self.run_.raw, "fx")
        self.assertIn("silent", info["check"])
        self.assertTrue(self.run_.done(self.run_.captures[0]))

    def test_the_page_warns_about_a_clip_whose_capture_was_kept_with_a_failed_check(
        self,
    ):
        clip = {"stem": "fx-a", "src": "fx", "bars": [0, 8]}
        info = {"tempo": 120.0, "seconds": 17.0}
        self.assertEqual(page.clip_warnings("I5", clip, info), [])
        (warning,) = page.clip_warnings(
            "I5", clip, dict(info, check="failed: FAIL silent in 1 bar(s)")
        )
        self.assertIn(
            "I5 clip fx-a: fx was kept with its check failed: FAIL silent", warning
        )

    def test_a_retry_that_differs_fails(self):
        ok, _ = self.capture_with([b"one", b"two"])
        self.assertFalse(ok)
        self.assertFalse(os.path.exists(os.path.join(self.run_.raw, "fx.wav")))
        self.assertTrue(self.run_.failed(self.run_.captures[0]))


def many(n, variant="base", start=0):
    return "".join(
        f"{variant}{i} | {variant} | pack | 8 | --seed {i + 1}\n"
        for i in range(start, start + n)
    )


class Splitting(unittest.TestCase):
    """#422: a variant holding most of the work is split across runners, each chunk its own claim."""

    def test_a_variant_with_most_of_the_captures_runs_on_every_runner(self):
        captures = jobs.merge(jobs.parse_jobs(many(22) + many(2, "s1"), "psytrance"))
        p = plan.plan(captures, runners=2, tempo_of=lambda c: 120.0)
        self.assertEqual(
            [any(c.variant == "base" for c in r.captures) for r in p.runners],
            [True, True],
        )
        self.assertEqual(sum(len(r.captures) for r in p.runners), 24)
        units = [
            u for u, _ in plan.groups(captures, {c.name: 17.0 for c in captures}, 2)
        ]
        self.assertEqual(sorted(units), ["base#1", "base#2", "s1"])
        # Each chunk keeps its own captures in Recipe order, and together they hold every base capture once
        chunks = dict(plan.groups(captures, {c.name: 17.0 for c in captures}, 2))
        self.assertEqual(
            sorted(c.name for c in chunks["base#1"] + chunks["base#2"]),
            sorted(f"base{i}" for i in range(22)),
        )

    def test_a_split_that_costs_more_links_than_it_saves_is_not_made(self):
        captures = jobs.merge(jobs.parse_jobs(ORDER_JOBS, genre="psytrance"))
        units = [
            u for u, _ in plan.groups(captures, {c.name: 17.0 for c in captures}, 2)
        ]
        self.assertEqual(sorted(units), ["base", "s1"])

    def test_one_runner_never_splits(self):
        captures = jobs.merge(jobs.parse_jobs(many(22), "psytrance"))
        units = [
            u for u, _ in plan.groups(captures, {c.name: 17.0 for c in captures}, 1)
        ]
        self.assertEqual(units, ["base"])

    def test_the_split_counts_only_what_is_left_to_capture(self):
        # A rerun where every s1 capture is current: base is all that's left, so it runs on both runners.
        with tempfile.TemporaryDirectory() as scratch:
            js = jobs.parse_jobs(many(6) + many(30, "s1"), "psytrance")
            r = runner.Run("psytrance", scratch, scratch, js, 2)
            r.tempos = {j.name: 120.0 for j in js}
            r.head = "abc"
            with mock.patch.object(
                runner.Run, "done", lambda self, c: c.variant == "s1"
            ):
                self.assertEqual(len(r.pending()), 6)
                p = r.plan()
            self.assertEqual([len(x.captures) for x in p.runners], [3, 3])

    def test_a_runner_holding_one_chunk_skips_another_runners_chunk(self):
        with tempfile.TemporaryDirectory() as scratch:
            js = jobs.parse_jobs(many(22), "psytrance")
            r = runner.Run("psytrance", scratch, scratch, js, 2)
            os.makedirs(r.raw)
            self.assertTrue(r.claim("base#1"))
            self.assertFalse(r.claim("base#1"))
            self.assertTrue(r.claim("base#2"))


def fake_render(folder, name, notes):
    """An executable that writes `notes` (a function of the seed) as its MIDI file, like generator_render --midi-only."""
    path = os.path.join(folder, name)
    with open(path, "w") as fh:
        fh.write(
            "#!/usr/bin/env python3\n"
            "import sys\n"
            "a = sys.argv\n"
            "seed = int(a[a.index('--seed') + 1])\n"
            f"notes = {notes!r}\n"
            "open(a[a.index('--out') + 1] + '.mid', 'w').write(str(eval(notes)))\n"
        )
    os.chmod(path, 0o755)
    return path


class Parents(unittest.TestCase):
    """#422: a variant stacked on another names it, and prove compares it against that one."""

    def test_make_records_each_variants_parent(self):
        with (
            tempfile.TemporaryDirectory() as repo,
            tempfile.TemporaryDirectory() as session,
        ):
            git_repo(repo)
            variants.make(
                repo,
                session,
                {
                    "s1": variants.Variant(edits=[("src/a.txt", "one", "two")]),
                    "s3": variants.Variant(
                        edits=[("src/a.txt", "one", "three")], parent="s1"
                    ),
                },
            )
            self.assertEqual(variants.parents(session), {"s3": "s1"})

    def test_make_refuses_an_unknown_parent(self):
        with (
            tempfile.TemporaryDirectory() as repo,
            tempfile.TemporaryDirectory() as session,
        ):
            git_repo(repo)
            with self.assertRaisesRegex(ValueError, "s9"):
                variants.make(
                    repo,
                    session,
                    {
                        "s3": variants.Variant(
                            edits=[("src/a.txt", "one", "3")], parent="s9"
                        )
                    },
                )

    def test_prove_compares_a_variant_with_its_parent(self):
        with tempfile.TemporaryDirectory() as scratch:
            renders = {
                "base": fake_render(scratch, "base", "'base'"),
                # s1 changes every seed; s3 changes s1 at even seeds only
                "s1": fake_render(scratch, "s1", "'s1'"),
                "s3": fake_render(scratch, "s3", "'s3' if seed % 2 == 0 else 's1'"),
            }
            lines = variants.compare(
                renders, ["s1", "s3"], {"s3": "s1"}, "dub", range(1, 5), 4, [], scratch
            )
        self.assertEqual(
            lines,
            [
                "s1: changes the notes at 4/4 seeds",
                "s3: changes the notes at 2/4 seeds against s1; nothing at seeds 1, 3",
            ],
        )

    def test_prove_builds_the_parents_render_too(self):
        self.assertEqual(
            variants.render_names(["s3", "s4"], {"s3": "s1", "s4": "s1"}),
            ["base", "s3", "s1", "s4"],
        )


class VariantCard(unittest.TestCase):
    """#422: a card takes a variant's pack/ (pack-edit variants), and extra overlay folders."""

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = tmp.name
        self.repo = os.path.join(self.tmp, "repo")
        os.makedirs(os.path.join(self.repo, "pack", "SYNTHS"))
        with open(os.path.join(self.repo, "pack", "SYNTHS", "SIREN.XML"), "w") as fh:
            fh.write("<rate>slow</rate>\n")
        git_repo(self.repo)
        self.stock = os.path.join(self.tmp, "stock.img")
        make_fat32(self.stock)
        self.scratch = os.path.join(self.tmp, "scratch")
        self.session = os.path.join(self.tmp, "session")
        with mock.patch.object(runner, "REPO", self.repo):
            variants.make(
                variants.scratch_worktree(
                    self.repo, os.path.join(self.scratch, "wt", "make")
                ),
                self.session,
                {
                    "fast": variants.Variant(
                        edits=[("pack/SYNTHS/SIREN.XML", "slow", "fast")]
                    )
                },
            )

    def card(self, *args):
        with (
            mock.patch.object(runner, "REPO", self.repo),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            cli.main(["card", "--scratch", self.scratch, "--stock", self.stock, *args])

    def read(self, name, path):
        import fat_image

        with fat_image.FatImage(os.path.join(self.scratch, f"card_{name}.img")) as card:
            return card.read_file(path)

    def test_a_variant_card_carries_the_variants_pack(self):
        self.card("--variant", "fast", "--session", self.session, "fast")
        self.assertEqual(self.read("fast", "SYNTHS/SIREN.XML"), b"<rate>fast</rate>\n")
        self.card("plain")
        self.assertEqual(self.read("plain", "SYNTHS/SIREN.XML"), b"<rate>slow</rate>\n")

    def test_a_variant_card_needs_the_session(self):
        with (
            self.assertRaises(SystemExit),
            contextlib.redirect_stderr(io.StringIO()),
        ):
            self.card("--variant", "fast", "fast")

    def test_an_overlay_folder_goes_over_the_pack(self):
        extra = os.path.join(self.tmp, "extra", "SYNTHS")
        os.makedirs(extra)
        with open(os.path.join(extra, "SIREN.XML"), "w") as fh:
            fh.write("<rate>deep</rate>\n")
        self.card("--overlay", os.path.dirname(extra), "deep")
        self.assertEqual(self.read("deep", "SYNTHS/SIREN.XML"), b"<rate>deep</rate>\n")


class DelugemuPin(unittest.TestCase):
    """#422: a run warns once when the installed delugemu isn't the pinned one, as gen_emu.py smoke does."""

    def test_the_warnings_are_gen_emus(self):
        pin = {"bundle": "1", "checkout": "a"}
        with (
            mock.patch.object(jobs.gen_emu, "read_pin", lambda: pin),
            mock.patch.object(
                jobs.gen_emu,
                "installed_versions",
                lambda qemu, delugemu: {"bundle": "2", "checkout": "a"},
            ),
        ):
            self.assertEqual(
                runner.delugemu_warnings(),
                jobs.gen_emu.pin_warnings(pin, {"bundle": "2", "checkout": "a"}),
            )

    def test_run_prints_them_once(self):
        class FakeRun:
            captures = ()

            def __init__(self, *a):
                pass

            def pending(self):
                return []

            def check(self):
                pass

            def plan(self):
                return types.SimpleNamespace(text=lambda: "plan")

        with tempfile.TemporaryDirectory() as tmp:
            jf = os.path.join(tmp, "jobs.txt")
            with open(jf, "w") as fh:
                fh.write("a | base | pack | 8 |\n")
            with (
                mock.patch.object(cli.runner, "Run", FakeRun),
                mock.patch.object(cli.runner, "delugemu_warnings", lambda: ["old"]),
                contextlib.redirect_stdout(io.StringIO()) as out,
            ):
                cli.main(
                    [
                        "run",
                        "--genre",
                        "dub",
                        "--session",
                        tmp,
                        "--scratch",
                        tmp,
                        "--dry-run",
                        jf,
                    ]
                )
        self.assertEqual(out.getvalue().count("WARNING old"), 1)


if __name__ == "__main__":
    unittest.main()
