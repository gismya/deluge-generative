"""Tests for deluge_probe.py's script runner against a fake Deluge that speaks the test port's sysex.

python3 scripts/generative/test_deluge_probe.py
"""

import json
import os
import re
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import deluge_probe as dp

STOPPED_IN_A_CLIP = {
    "ui": "instrument_clip",
    "root_ui": "instrument_clip",
    "ui_mode": 0,
    "playing": False,
    "inject_queue": 0,
    "sd_routine_lock": False,
    "accessing_card": False,
    "sample_timer": 44100,
}


class FakeDeluge:
    """A link to a build already in start state. It answers state requests, and each inject with "queued", or with
    "parse error" for the words the build doesn't know. An inject listed in `events` sends those test events (a list,
    or a function that returns one each time), each after a timestamp as the firmware's TestPort::event() writes it
    ("%.4f: "): 1.0001, 1.0002 and so on. "reset recipe" answers gen_recipe_reset done unless `events` says otherwise.
    `held` is what "hold NAME" lines have set and no "release NAME" or "release all" has let go. `event_queue` is the
    lines still in the firmware's event queue, as the events each drops: a state request answers how many there are
    (state's event_queue), then sends one, adding its drops to events_dropped."""

    def __init__(self, unknown=(), events=None, silent=None):
        self.unknown = set(unknown)
        self.events = {"reset recipe": ["gen_recipe_reset done"], **(events or {})}
        self.replies, self.injected, self.held = [], [], set()
        self.sent_events = 0
        self.event_queue = []
        self.silent = silent  # () -> whether state requests go unanswered now (a song load blocks the test port)
        self.state = dict(
            STOPPED_IN_A_CLIP
        )  # what state requests answer; a test may change it

    def send(self, msg):
        if msg[:5] != dp.HDR or msg[5] != 0x02:
            return  # log subscription and the like
        command, seq, body = msg[6], msg[7], bytes(msg[8:-1]).decode("ascii")
        if command == dp.STATE:
            if self.silent is not None and self.silent():
                return
            text = json.dumps(
                dict(self.state, event_queue=len(self.event_queue))
            ).encode("ascii")
            if self.event_queue:
                self.state["events_dropped"] = self.state.get(
                    "events_dropped", 0
                ) + self.event_queue.pop(0)
            self.replies.append([*dp.HDR, 0x02, dp.STATE_REPLY, seq, *text, 0xF7])
            return
        word = " ".join(body.split()[:2])
        known = not any(word.startswith(w) for w in self.unknown)
        self.replies.append(
            [*dp.HDR, 0x02, dp.STATUS_REPLY, seq, 0 if known else 2, 0xF7]
        )
        if known:
            self.injected.append(body)
            verb, _, name = body.partition(" ")
            if verb == "hold":
                self.held.add(name)
            elif body == "release all":
                self.held.clear()
            elif verb == "release":
                self.held.discard(name)
            lines = self.events.get(body, [])
            for line in lines() if callable(lines) else lines:
                self.sent_events += 1
                line = f"{1 + self.sent_events / 10000:.4f}: {line}"
                self.replies.append([*dp.EVENT_HDR, *line.encode("ascii"), 0xF7])

    @property
    def script_injected(self):
        """`injected` without run_script's closing "release all" (ScriptEnd)."""
        return (
            self.injected[:-1]
            if self.injected[-1:] == ["release all"]
            else self.injected
        )

    def send_midi(self, msg):
        pass

    def receive(self, timeout):
        return self.replies.pop(0) if self.replies else None

    def close(self):
        pass


def run(link, *lines):
    """(failed, total, out) of run_script(). A script error raises the error that stopped it (ScriptStopped's)."""
    out = []
    try:
        failed, total = dp.run_script(dp.TestPort(link), list(lines), out=out.append)
    except dp.ScriptStopped as exc:
        raise exc.error from exc
    return failed, total, out


class StartState(unittest.TestCase):
    def test_a_build_in_start_state_passes(self):
        link = FakeDeluge()
        self.assertEqual(run(link, "start_state")[:2], (0, 1))
        self.assertEqual(link.script_injected, ["release all", "reset recipe"])

    def test_it_releases_what_a_stopped_script_left_held(self):
        # A hold the probe has never heard of: only the firmware lists them. A stopped probe (a crash, a lost link)
        # can leave holds on; run_script's own end lets go of them too (ScriptEnd).
        link = FakeDeluge()
        link.held = {"card_error", "a_new_one"}
        failed, total, out = run(link, "start_state")
        self.assertEqual((failed, total, out), (0, 1, ["PASS line 1: start_state"]))
        self.assertEqual(link.held, set())

    def test_a_build_older_than_release_all_gets_each_hold_released(self):
        link = FakeDeluge(unknown=["release all"])
        link.held = {"card_error", "midi_fill"}
        failed, total, out = run(link, "start_state")
        self.assertEqual((failed, total, out), (0, 1, ["PASS line 1: start_state"]))
        self.assertEqual(link.held, set())

    def test_a_build_older_than_release_entry_passes_with_a_warning(self):
        failed, total, out = run(
            FakeDeluge(unknown=["release all", "release entry"]), "start_state"
        )
        self.assertEqual((failed, total), (0, 1))
        self.assertTrue(
            any(
                line.startswith("WARNING line 1:") and "release entry" in line
                for line in out
            ),
            out,
        )

    def test_a_build_older_than_reset_recipe_passes_with_a_warning(self):
        # Bisecting: the Current Recipe stays as the last script left it, and the script still runs.
        link = FakeDeluge(unknown=["reset recipe"])
        failed, total, out = run(link, "start_state")
        self.assertEqual((failed, total), (0, 1))
        self.assertEqual(link.script_injected, ["release all"])
        warnings = [line for line in out if line.startswith("WARNING line 1:")]
        self.assertEqual(len(warnings), 1, out)
        self.assertIn("reset recipe", warnings[0])

    def test_a_refused_reset_fails_start_state(self):
        link = FakeDeluge(
            events={"reset recipe": ["gen_recipe_reset refused why=perform"]}
        )
        failed, total, out = run(link, "start_state")
        self.assertEqual((failed, total), (1, 1))
        self.assertIn("refused why=perform", out[0])

    def test_a_reset_refused_while_a_job_ends_is_retried(self):
        # Right after a leave, the runner's last job may still be running.
        answers = iter(
            [["gen_recipe_reset refused why=busy"], ["gen_recipe_reset done"]]
        )
        link = FakeDeluge(events={"reset recipe": lambda: next(answers)})
        self.assertEqual(run(link, "start_state")[:2], (0, 1))
        self.assertEqual(
            link.script_injected, ["release all", "reset recipe", "reset recipe"]
        )

    def test_a_reset_that_stays_busy_fails_start_state_in_time(self):
        link = FakeDeluge(
            events={"reset recipe": ["gen_recipe_reset refused why=busy"]}
        )
        with mock.patch.object(dp, "time", FakeClock()):
            failed, total, out = run(link, "start_state")
        self.assertEqual((failed, total), (1, 1))
        self.assertIn("refused why=busy", out[0])

    def test_a_busy_retry_that_crosses_the_deadline_still_reads_its_answer(self):
        clock = FakeClock()

        def late_done():
            clock.now += 20  # the retry's inject went out just before the deadline; its answer comes after it
            return ["gen_recipe_reset done"]

        answers = iter([lambda: ["gen_recipe_reset refused why=busy"], late_done])
        link = FakeDeluge(events={"reset recipe": lambda: next(answers)()})
        with mock.patch.object(dp, "time", clock):
            failed, total, out = run(link, "start_state")
        self.assertEqual((failed, total), (0, 1), out)

    def test_a_reset_with_no_event_fails_start_state(self):
        link = FakeDeluge(events={"reset recipe": []})
        with mock.patch.object(dp, "time", FakeClock()):
            failed, total, out = run(link, "start_state")
        self.assertEqual((failed, total), (1, 1))
        self.assertIn("no gen_recipe_reset event", out[0])

    def test_a_reset_recipe_line_is_queued_on_the_firmware(self):
        link = FakeDeluge()
        self.assertEqual(
            run(link, "reset recipe", "expect event gen_recipe_reset done")[:2], (0, 1)
        )
        self.assertEqual(link.script_injected, ["reset recipe"])

    def test_another_rejected_inject_still_stops_the_script(self):
        with self.assertRaises(RuntimeError):
            run(
                FakeDeluge(unknown=["button SHIFT"]), "start_state", "button SHIFT down"
            )


class ScriptEnd(unittest.TestCase):
    """A script's holds end with it: "hold entropy" left on would pin every later entry until a reboot."""

    def test_a_script_that_stops_on_an_error_lets_go_of_its_holds(self):
        link = FakeDeluge()
        with self.assertRaisesRegex(ValueError, "unknown command"):
            run(link, "start_state", "hold entropy", "no such")
        self.assertEqual(link.held, set())

    def test_a_probe_bug_inside_a_script_still_lets_go_of_its_holds(self):
        # A LookupError inside a script is a probe bug, not a lost link, so the release still goes out.
        link = FakeDeluge()
        original = dp._run_lines

        def run_lines(*args, **kwargs):
            original(*args, **kwargs)
            raise KeyError("probe bug")

        with (
            unittest.mock.patch.object(dp, "_run_lines", run_lines),
            self.assertRaises(KeyError),
        ):
            run(link, "start_state", "hold entropy")
        self.assertEqual(link.held, set())

    def test_a_script_that_ends_holding_lets_go(self):
        link = FakeDeluge()
        self.assertEqual(run(link, "start_state", "hold entropy")[:2], (0, 1))
        self.assertEqual(link.held, set())
        self.assertEqual(link.injected[-1], "release all")

    def test_on_a_build_older_than_release_all_it_says_nothing_more(self):
        # start_state already warned about each hold the build doesn't know.
        link = FakeDeluge(unknown=["release all", "release entry"])
        _, _, out = run(link, "hold card_error")
        self.assertEqual((out, link.held), ([], set()))

    def test_a_lost_link_keeps_the_scripts_own_error(self):
        link = FakeDeluge()
        original_send = link.send

        def send(msg):
            if msg[5:7] == [0x02, dp.INJECT] and b"release" in bytes(msg[8:-1]):
                raise OSError("link lost")
            original_send(msg)

        link.send = send
        with self.assertRaisesRegex(ValueError, "unknown command"):
            run(link, "no such")

    def test_a_lost_link_skips_the_release(self):
        # A hang: the release would only add an inject's wait (up to 10 s on QUEUE_FULL) before the error shows.
        link, sent, hung = FakeDeluge(), [], []
        original_send = link.send

        def send(msg):
            if msg[5:7] == [0x02, dp.INJECT]:
                sent.append(bytes(msg[8:-1]).decode("ascii"))
            if not hung:
                original_send(msg)
            if sent[-1:] == ["button BACK down"]:
                hung.append(True)

        link.send = send
        with self.assertRaisesRegex(TimeoutError, "no reply"):
            run(link, "hold entropy", "button BACK down", "button BACK up")
        self.assertEqual(sent, ["hold entropy", "button BACK down", "button BACK up"])


class DroppedEvents(unittest.TestCase):
    def dropping(self, link, n, queued=0):
        """An inject's events: gen_play, while the firmware drops `n` more, `queued` lines later (see FakeDeluge)."""

        def play():
            if queued:
                link.event_queue.extend([0] * (queued - 1) + [n])
            else:
                link.state["events_dropped"] = link.state.get("events_dropped", 0) + n
            return ["gen_play"]

        return play

    def test_a_script_fails_once_when_the_firmware_dropped_events_since_start_state(
        self,
    ):
        link = FakeDeluge()
        link.events["button PLAY down"] = self.dropping(link, 2)
        link.state["events_dropped"] = 3  # from before the script: not its own
        failed, total, out = run(
            link, "start_state", "button PLAY down", "expect event gen_play"
        )
        self.assertEqual((failed, total), (1, 3))
        self.assertTrue(
            out[-1].startswith("FAIL end: 2 test events dropped since start_state:")
        )

    def test_nothing_is_added_when_none_was_dropped(self):
        link = FakeDeluge()
        link.state["events_dropped"] = 3
        self.assertEqual(run(link, "start_state"), (0, 1, ["PASS line 1: start_state"]))

    def test_drops_before_a_later_start_state_count_too(self):
        link = FakeDeluge()
        link.events["button PLAY down"] = self.dropping(link, 2)
        failed, total, out = run(link, "start_state", "button PLAY down", "start_state")
        self.assertEqual((failed, total), (1, 3))
        self.assertTrue(out[-1].startswith("FAIL end: 2 test events dropped"))

    def test_a_drop_while_the_last_lines_still_wait_to_be_sent_counts(self):
        link = FakeDeluge()
        link.events["button PLAY down"] = self.dropping(link, 2, queued=8)
        failed, total, out = run(link, "start_state", "button PLAY down")
        self.assertEqual((failed, total), (1, 2))
        self.assertTrue(out[-1].startswith("FAIL end: 2 test events dropped"))

    def test_a_drop_while_start_states_own_lines_wait_to_be_sent_isnt_the_scripts(
        self,
    ):
        link = FakeDeluge()

        def reset():
            link.event_queue.extend([0] * 7 + [2])
            return ["gen_recipe_reset done"]

        link.events["reset recipe"] = reset
        self.assertEqual(run(link, "start_state"), (0, 1, ["PASS line 1: start_state"]))

    def test_a_firmware_reset_counts_its_drops_from_zero(self):
        # events_dropped counts from boot: after a reset mid-script it falls, and its new value is all new drops.
        link = FakeDeluge()
        link.state["events_dropped"] = 5

        def reboot():
            link.state["events_dropped"] = 2
            return ["gen_play"]

        link.events["button PLAY down"] = reboot
        failed, total, out = run(
            link, "start_state", "button PLAY down", "expect event gen_play"
        )
        self.assertEqual((failed, total), (1, 3))
        self.assertTrue(out[-1].startswith("FAIL end: 2 test events dropped"), out)

    def test_drops_with_a_hold_kept_events_and_a_window_in_one_script(self):
        # #280's drop check and #281's keep events, holds and within= together: the end's release comes after the
        # drop check's last state poll, and the drops count once.
        link, order = FakeDeluge(), []
        link.events["button PLAY down"] = self.dropping(link, 2, queued=3)
        original_send = link.send

        def send(msg):
            if msg[5:7] == [0x02, dp.STATE]:
                order.append("state")
            elif msg[5:7] == [0x02, dp.INJECT]:
                order.append(bytes(msg[8:-1]).decode("ascii"))
            original_send(msg)

        link.send = send
        failed, total, out = run(
            link,
            "start_state",
            "hold entropy",
            "mark",
            "button PLAY down",
            "keep events gen_play",
            "expect events kept within=1",
            "expect event gen_play since=mark within=1",
        )
        self.assertEqual((failed, total), (1, 4), out)
        self.assertTrue(out[-1].startswith("FAIL end: 2 test events dropped"), out)
        self.assertEqual(order[-2:], ["state", "release all"])
        self.assertEqual(link.held, set())


class FakeClock:
    """Virtual time for deluge_probe's `time` module: each look at the clock moves it on 10 ms, a sleep by its length."""

    def __init__(self):
        self.now = 0.0

    def time(self):
        self.now += 0.01
        return self.now

    monotonic = time

    def sleep(self, seconds):
        self.now += seconds


class LoadSong(unittest.TestCase):
    def test_an_expect_after_a_load_waits_for_the_port_as_long_as_its_within(self):
        # The load blocks the test port for 15 s, past the 10 s an expect waits for the queue by default.
        clock, loaded_at = FakeClock(), []

        def loading():
            if "load song SONGS/BIG.XML" in link.injected and not loaded_at:
                loaded_at.append(clock.now)
            return bool(loaded_at) and clock.now < loaded_at[0] + 15

        link = FakeDeluge(
            events={
                "load song SONGS/BIG.XML": ["song_loaded path=SONGS/BIG.XML ms=15000"]
            },
            silent=loading,
        )
        with mock.patch.object(dp, "time", clock):
            failed, total, out = run(
                link, "load song SONGS/BIG.XML", "expect event song_loaded within=60"
            )
        self.assertEqual((failed, total), (0, 1), out)

    def test_a_load_song_line_goes_to_the_firmware_whole(self):
        link = FakeDeluge()
        self.assertEqual(run(link, "load song SONGS/MY SONG.XML")[:2], (0, 0))
        self.assertEqual(link.script_injected, ["load song SONGS/MY SONG.XML"])

    def test_a_report_input_line_is_queued_on_the_firmware(self):
        link = FakeDeluge(
            events={
                "report input": ["input_lag ok=yes max_ms=3 presses=2 gold_gap_ms=0"]
            }
        )
        self.assertEqual(
            run(link, "report input", "expect event input_lag ok=yes")[:2], (0, 1)
        )
        self.assertEqual(link.script_injected, ["report input"])


class ExpectTimeout(unittest.TestCase):
    """within=S counts from the expect line, the wait for the queue included: a failing expect takes about S, not
    the queue's wait and then S more."""

    def run_busy(self, *lines, busy_for=3.0):
        clock = FakeClock()
        link = FakeDeluge()
        state = link.state
        original_send = link.send

        def send(msg):
            state["inject_queue"] = 1 if clock.now < busy_for else 0
            original_send(msg)

        link.send = send
        with mock.patch.object(dp, "time", clock):
            failed, _, out = run(link, *lines)
        return failed, clock.now, out

    def test_a_failing_event_expect_ends_within_its_window(self):
        failed, now, _ = self.run_busy("expect event gen_left within=5")
        self.assertEqual(failed, 1)
        self.assertLess(now, 5.5)

    def test_a_failing_state_expect_ends_within_its_window(self):
        failed, now, _ = self.run_busy("expect playing=true within=5")
        self.assertEqual(failed, 1)
        self.assertLess(now, 5.5)

    def test_a_queue_longer_than_the_window_still_gets_one_look(self):
        failed, _, out = self.run_busy("expect playing=false within=2", busy_for=3)
        self.assertEqual(failed, 0, out)

    def test_no_event_still_watches_its_whole_window_after_the_queue(self):
        _, now, _ = self.run_busy("expect no_event gen_left within=5")
        self.assertGreater(now, 7.9)

    def test_a_failure_after_a_queue_longer_than_the_window_says_so(self):
        failed, _, out = self.run_busy("expect event gen_left within=2", busy_for=3)
        self.assertEqual(failed, 1)
        self.assertRegex(
            out[-1],
            r"^  the queue before it took 3\.\d s of its within=2: the expect got only a last look$",
        )

    def test_a_failure_whose_queue_left_less_than_the_last_look_says_so(self):
        # The queue took 1.9 s of 2: the expect looked only LAST_LOOK (0.2 s), as when the queue took it all.
        failed, _, out = self.run_busy("expect event gen_left within=2", busy_for=1.9)
        self.assertEqual(failed, 1)
        self.assertRegex(out[-1], r"^  the queue before it took 1\.9 s of its within=2")

    def test_a_short_within_after_a_long_queue_says_so(self):
        # within=0.1 gets only the last look anyway, but a queue that took seconds is still why it failed.
        failed, _, out = self.run_busy("expect event gen_left within=0.1", busy_for=3)
        self.assertEqual(failed, 1)
        self.assertRegex(
            out[-1], r"^  the queue before it took 3\.\d s of its within=0.1"
        )

    def test_a_short_within_with_no_queue_doesnt_blame_the_queue(self):
        failed, _, out = self.run_busy("expect event gen_left within=0.1", busy_for=0)
        self.assertEqual(failed, 1)
        self.assertFalse(any("the queue" in line for line in out), out)

    def test_a_failure_inside_its_window_doesnt_blame_the_queue(self):
        failed, _, out = self.run_busy("expect event gen_left within=5", busy_for=3)
        self.assertEqual(failed, 1)
        self.assertFalse(any("the queue" in line for line in out), out)


class ExpectNoEventFailure(unittest.TestCase):
    def test_a_failed_no_event_prints_the_event_that_came(self):
        link = FakeDeluge(events={"button BACK down": ["gen_left delete_us=300"]})
        failed, _, out = run(
            link, "button BACK down", "expect no_event gen_left within=0.1"
        )
        self.assertEqual(failed, 1)
        self.assertEqual(out[1:], ["  got 1.0001: gen_left delete_us=300"])


class ExpectEventCaptures(unittest.TestCase):
    """A pattern's named group keeps what it matched, and a later pattern's (?P=name) matches that text: a value the
    script can't know ahead (a rolled tempo) checked against where it turns up again."""

    def link(self, later):
        return FakeDeluge(
            events={
                "button PLAY down": ["gen_pending tempo=12.5"],
                "button PLAY up": [later],
            }
        )

    def test_a_later_pattern_matches_what_an_earlier_one_captured(self):
        failed, total, _ = run(
            self.link("gen_start tempo=12.5"),
            "button PLAY down",
            "button PLAY up",
            r"expect event /gen_pending tempo=(?P<tempo>[\d.]+)$/",
            "expect event /gen_start tempo=(?P=tempo)$/",
        )
        self.assertEqual((failed, total), (0, 2))

    def test_fails_on_another_value_and_matches_the_capture_literally(self):
        failed, _, _ = run(
            self.link("gen_start tempo=1295"),
            "button PLAY down",
            "button PLAY up",
            r"expect event /gen_pending tempo=(?P<tempo>[\d.]+)$/",
            "expect event /gen_start tempo=(?P=tempo)$/ within=0.1",
        )
        self.assertEqual(failed, 1)

    def test_a_name_never_captured_is_a_script_error(self):
        with self.assertRaisesRegex(ValueError, "line 2: .*tempo"):
            run(
                self.link("gen_start tempo=12.5"),
                "button PLAY down",
                "expect event /gen_pending tempo=(?P=tempo)$/",
            )

    def test_a_capture_whose_expect_failed_fails_the_expect_that_reads_it_and_the_script_goes_on(
        self,
    ):
        failed, total, out = run(
            self.link("gen_start tempo=12.5"),
            "button PLAY down",
            r"expect event /gen_pending speed=(?P<tempo>[\d.]+)$/ within=0.1",
            "button PLAY up",
            "expect event /gen_start tempo=(?P=tempo)$/",
            "expect no_event /gen_start tempo=(?P=tempo)$/",
            "expect next event /gen_start tempo=(?P=tempo)$/",
            "expect event gen_start",
        )
        self.assertEqual((failed, total), (4, 5), out)
        for number in (4, 5, 6):
            line = next(o for o in out if o.startswith(f"FAIL line {number}:"))
            self.assertIn("no tempo captured: the expect that captures it failed", line)
        self.assertTrue(any(o.startswith("PASS line 7:") for o in out), out)

    def test_a_capture_a_later_expect_makes_is_read_after_its_first_expect_failed(self):
        failed, total, out = run(
            self.link("gen_start tempo=12.5"),
            "button PLAY down",
            r"expect event /gen_pending speed=(?P<tempo>[\d.]+)$/ within=0.1",
            r"expect event /gen_pending tempo=(?P<tempo>[\d.]+)$/",
            "button PLAY up",
            "expect event /gen_start tempo=(?P=tempo)$/",
        )
        self.assertEqual((failed, total), (1, 3), out)

    def test_start_state_drops_the_captures(self):
        with self.assertRaisesRegex(ValueError, "line 5: .*tempo"):
            run(
                self.link("gen_start tempo=12.5"),
                "button PLAY down",
                r"expect event /gen_pending tempo=(?P<tempo>[\d.]+)$/",
                "start_state",
                "button PLAY up",
                "expect event /gen_start tempo=(?P=tempo)$/",
            )

    def test_each_run_script_starts_with_nothing_captured(self):
        link = self.link("gen_start tempo=12.5")
        run(
            link,
            "button PLAY down",
            r"expect event /gen_pending tempo=(?P<tempo>[\d.]+)$/",
        )
        with self.assertRaisesRegex(ValueError, "line 2: .*tempo"):
            run(link, "button PLAY up", "expect event /gen_start tempo=(?P=tempo)$/")

    def test_a_capture_reaches_an_included_scripts_lines(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        with open(os.path.join(tmp.name, "start.txt"), "w") as f:
            f.write("button PLAY up\nexpect event /gen_start tempo=(?P=tempo)$/\n")
        script = os.path.join(tmp.name, "main.txt")
        with open(script, "w") as f:
            f.write(
                "button PLAY down\n"
                "expect event /gen_pending tempo=(?P<tempo>[\\d.]+)$/\n"
                "include start.txt\n"
            )
        out = []
        failed, total = dp.run_script(
            dp.TestPort(self.link("gen_start tempo=12.5")),
            dp.read_script(script),
            out=out.append,
        )
        self.assertEqual((failed, total), (0, 2), out)

    def test_a_patterns_own_group_and_backreference_are_left_alone(self):
        failed, total, _ = run(
            FakeDeluge(events={"button PLAY down": ["gen_pair a=5 b=5"]}),
            "button PLAY down",
            r"expect event /gen_pair a=(?P<v>\d) b=(?P=v)$/",
        )
        self.assertEqual((failed, total), (0, 1))

    def test_an_escaped_backreference_is_left_alone(self):
        # A group holding an optional "(" then "P=tempo": no capture is looked up
        failed, total, _ = run(
            FakeDeluge(events={"button PLAY down": ["gen_odd (P=tempo"]}),
            "button PLAY down",
            r"expect event /gen_odd (\(?P=tempo)$/",
        )
        self.assertEqual((failed, total), (0, 1))


class ExpectEventFailure(unittest.TestCase):
    """A failed "expect event" prints the recent events of its kind (the same first word after the firmware's
    timestamp), so one run shows why."""

    def setUp(self):
        self.link = FakeDeluge(
            events={
                "button BACK down": [
                    "gen_left delete_us=300",
                    "gen_press solo part=1",
                ],
                "button BACK up": ["gen_left delete_us=310"],
            }
        )

    def test_prints_the_events_of_the_same_kind_it_looked_at(self):
        failed, _, out = run(
            self.link,
            "button BACK down",
            "button BACK up",
            "expect event gen_left delete_us=999 within=0.1",
        )
        self.assertEqual(failed, 1)
        self.assertEqual(
            out[1:],
            [
                "  got 1.0001: gen_left delete_us=300",
                "  got 1.0003: gen_left delete_us=310",
            ],
        )

    def test_marks_the_ones_an_earlier_expect_already_matched(self):
        _, _, out = run(
            self.link,
            "button BACK down",
            "expect event gen_left",
            "expect event gen_left delete_us=300 within=0.1",
        )
        self.assertEqual(
            out[2:], ["  got 1.0001: gen_left delete_us=300 (line 2's match)"]
        )

    def test_marks_those_before_the_earlier_match(self):
        _, _, out = run(
            self.link,
            "button BACK down",
            "button BACK up",
            "expect event gen_left delete_us=310",
            "expect event gen_left delete_us=999 within=0.1",
        )
        self.assertEqual(
            out[2:],
            [
                "  got 1.0001: gen_left delete_us=300 (before line 3's match)",
                "  got 1.0003: gen_left delete_us=310 (line 3's match)",
            ],
        )

    def test_text_that_isnt_a_kind_prints_the_events_containing_its_first_word(self):
        _, _, out = run(
            self.link, "button BACK down", "expect event left delete_us=999 within=0.1"
        )
        self.assertEqual(out[1:], ["  got 1.0001: gen_left delete_us=300"])

    def test_looks_only_at_events_from_the_scripts_start_state_on(self):
        _, _, out = run(
            self.link,
            "button BACK down",
            "start_state",
            "button BACK up",
            "expect event gen_left delete_us=999 within=0.1",
        )
        # 1.0002 is start_state's gen_recipe_reset
        self.assertEqual(out[2:], ["  got 1.0004: gen_left delete_us=310"])

    def test_says_when_none_of_that_kind_arrived(self):
        _, _, out = run(
            self.link, "button BACK down", "expect event gen_mem within=0.1"
        )
        self.assertEqual(out[1:], ["  got no gen_mem event"])

    def test_a_pass_prints_nothing_more(self):
        _, _, out = run(self.link, "button BACK down", "expect event gen_left")
        self.assertEqual(out, ["PASS line 2: expect event gen_left"])


class Mark(unittest.TestCase):
    """ "mark" keeps where the next "expect event" would search from; "since=mark" searches from there instead. Events
    a frame reports in either order then match in either order (perform_song.txt's Arc map rows)."""

    def setUp(self):
        self.link = FakeDeluge(
            events={
                "button PLAY down": [
                    "gen_arcrow 0 ...np",
                    "gen_arcmap s=...GGGG",
                    "gen_arcrow 1 ...np",
                ],
            }
        )

    def test_an_event_before_the_last_match_matches_since_the_mark(self):
        failed, total, _ = run(
            self.link,
            "mark",
            "button PLAY down",
            "expect event gen_arcmap s=...GGGG",
            "expect event gen_arcrow 0 ...np since=mark within=0.1",
        )
        self.assertEqual((failed, total), (0, 2))

    def test_without_since_it_searches_after_the_last_match(self):
        failed, _, _ = run(
            self.link,
            "mark",
            "button PLAY down",
            "expect event gen_arcmap s=...GGGG",
            "expect event gen_arcrow 0 ...np within=0.1",
        )
        self.assertEqual(failed, 1)

    def test_a_match_since_the_mark_doesnt_move_the_search_back(self):
        failed, _, out = run(
            self.link,
            "mark",
            "button PLAY down",
            "expect event gen_arcrow 1",
            "expect event gen_arcrow 0 since=mark",
            "expect event gen_arcmap within=0.1",
        )
        self.assertEqual(failed, 1, out)

    def test_since_and_within_come_in_either_order(self):
        failed, _, _ = run(
            self.link,
            "mark",
            "button PLAY down",
            "expect event gen_arcmap",
            "expect event gen_arcrow 0 within=0.1 since=mark",
        )
        self.assertEqual(failed, 0)

    def test_no_event_since_the_mark_sees_one_before_the_last_match(self):
        failed, _, out = run(
            self.link,
            "mark",
            "button PLAY down",
            "expect event gen_arcmap",
            "expect no_event gen_arcrow 0 since=mark within=0.1",
        )
        self.assertEqual(failed, 1, out)

    def test_no_event_without_since_misses_one_before_the_last_match(self):
        failed, _, out = run(
            self.link,
            "mark",
            "button PLAY down",
            "expect event gen_arcmap",
            "expect no_event gen_arcrow 0 within=0.1",
        )
        self.assertEqual(failed, 0, out)

    def test_since_without_a_mark_is_a_script_error(self):
        with self.assertRaisesRegex(ValueError, "no mark"):
            run(self.link, "expect event gen_arcmap since=mark")

    def test_start_state_drops_the_mark(self):
        with self.assertRaisesRegex(ValueError, "no mark"):
            run(self.link, "mark", "start_state", "expect event gen_arcmap since=mark")

    def test_no_event_since_without_a_mark_is_a_script_error(self):
        with self.assertRaisesRegex(ValueError, "no mark"):
            run(self.link, "expect no_event gen_arcmap since=mark")

    def test_since_on_a_form_that_doesnt_search_events_is_a_script_error(self):
        for line in (
            "expect playing=false since=mark",
            "expect oled_changed since=mark",
            "expect playing=kept since=mark",
        ):
            with (
                self.subTest(line=line),
                self.assertRaisesRegex(ValueError, "since=mark only"),
            ):
                run(self.link, "mark", line)

    def test_start_state_drops_the_mark_for_no_event_too(self):
        with self.assertRaisesRegex(ValueError, "no mark"):
            run(
                self.link,
                "mark",
                "start_state",
                "expect no_event gen_arcmap since=mark",
            )


class Keep(unittest.TestCase):
    """ "keep KEY..." keeps state values; "expect KEY=kept" compares with them (perform_runner.txt: BACK during entry
    leaves the song's tempo and undo log as they were)."""

    def setUp(self):
        self.link = FakeDeluge()
        self.link.state.update(bpm=123.5, undo_log=2, playing=True)

    def test_a_value_as_kept_passes(self):
        failed, total, out = run(
            self.link,
            "keep bpm undo_log playing",
            "expect bpm=kept",
            "expect undo_log=kept",
            "expect playing=kept",
        )
        self.assertEqual((failed, total), (0, 3), out)

    def test_a_value_that_moved_fails(self):
        def press():
            self.link.state["undo_log"] = 3
            return []

        self.link.events["button PLAY down"] = press
        failed, total, out = run(
            self.link,
            "keep bpm undo_log",
            "button PLAY down",
            "expect bpm=kept",
            "expect undo_log=kept within=0.1",
        )
        self.assertEqual((failed, total), (1, 2), out)
        self.assertIn("got 3", out[-1])

    def test_kept_without_keep_is_a_script_error(self):
        with self.assertRaisesRegex(ValueError, "no keep"):
            run(self.link, "expect bpm=kept")

    def test_start_state_drops_what_keep_kept(self):
        with self.assertRaisesRegex(ValueError, "no keep"):
            run(self.link, "keep bpm", "start_state", "expect bpm=kept")

    def test_keep_of_a_missing_key_is_a_script_error(self):
        with self.assertRaisesRegex(ValueError, "no state key"):
            run(self.link, "keep tempo")


class KeepEvents(unittest.TestCase):
    """ "keep events TEXT" keeps the events matching TEXT since the mark; "expect events kept" passes once the events
    since the (next) mark are the same, in order: two entries under "hold entropy" load the same sounds, whichever
    ones the card has (perform_entropy.txt)."""

    def entries(self, *rolls):
        """A link whose each SELECT press loads the next roll's sounds."""
        rolls = list(rolls)
        return FakeDeluge(
            events={
                "button 4 3 down": lambda: [
                    *(f"gen_sound {sound}" for sound in rolls.pop(0)),
                    "gen_perform_entered techno-acid",
                ]
            }
        )

    def run_entries(self, link, *expect):
        return run(
            link,
            "mark",
            "button 4 3 down",
            "expect event gen_perform_entered",
            "keep events /^gen_sound /",
            "mark",
            "button 4 3 down",
            *expect,
        )

    def test_the_same_events_pass(self):
        link = self.entries(["KICK a.wav", "ACID b.XML"], ["KICK a.wav", "ACID b.XML"])
        failed, _, out = self.run_entries(link, "expect events kept within=0.1")
        self.assertEqual(failed, 0, out)

    def test_another_event_fails_and_shows_the_first_difference(self):
        link = self.entries(["KICK a.wav", "ACID b.XML"], ["KICK a.wav", "ACID c.XML"])
        failed, _, out = self.run_entries(link, "expect events kept within=0.1")
        self.assertEqual(failed, 1)
        self.assertEqual(
            out[2:],
            [
                "  kept 2 /^gen_sound / events, got 2; the first that differs:",
                "  kept gen_sound ACID b.XML",
                "  got gen_sound ACID c.XML",
            ],
        )

    def test_a_missing_event_fails(self):
        link = self.entries(["KICK a.wav", "ACID b.XML"], ["KICK a.wav"])
        failed, _, out = self.run_entries(link, "expect events kept within=0.1")
        self.assertEqual(failed, 1)
        self.assertEqual(
            out[2:4],
            [
                "  kept 2 /^gen_sound / events, got 1; the first that differs:",
                "  kept gen_sound ACID b.XML",
            ],
        )
        self.assertEqual(out[4], "  got none")

    def test_an_extra_event_fails(self):
        link = self.entries(["KICK a.wav"], ["KICK a.wav", "ACID b.XML"])
        failed, _, _ = self.run_entries(link, "expect events kept within=0.1")
        self.assertEqual(failed, 1)

    def test_expect_events_kept_matches_with_the_captures_of_its_keep_events(self):
        # keep events reads (?P=v) as 1; v is captured again as 2 before expect events kept, which still picks v=1's.
        link = FakeDeluge(
            events={
                "button PLAY down": ["gen_sound 1 a", "gen_sound 2 b", "gen_pick v=1"],
                "button PLAY up": ["gen_sound 1 a", "gen_sound 2 c", "gen_pick v=2"],
            }
        )
        failed, total, out = run(
            link,
            "mark",
            "button PLAY down",
            r"expect event /^gen_pick v=(?P<v>\d)$/",
            "keep events /^gen_sound (?P=v) /",
            "mark",
            "button PLAY up",
            r"expect event /^gen_pick v=(?P<v>\d)$/",
            "expect events kept within=0.1",
        )
        self.assertEqual((failed, total), (0, 3), out)

    def test_keep_events_without_a_mark_is_a_script_error(self):
        with self.assertRaisesRegex(ValueError, "line 1: keep events with no mark"):
            run(FakeDeluge(), "keep events gen_sound")

    def test_expect_events_kept_without_keep_events_is_a_script_error(self):
        with self.assertRaisesRegex(
            ValueError, "line 2: expect events kept with no keep events"
        ):
            run(FakeDeluge(), "mark", "expect events kept")

    def test_start_state_drops_the_kept_events(self):
        with self.assertRaisesRegex(
            ValueError, "line 5: expect events kept with no keep events"
        ):
            run(
                self.entries(["kick"]),
                "mark",
                "button 4 3 down",
                "keep events gen_sound",
                "start_state",
                "expect events kept",
            )

    def test_keep_events_that_kept_none_is_a_script_error(self):
        """Else "expect events kept" would pass on two empty lists."""
        with self.assertRaisesRegex(
            ValueError, "line 2: keep events kept no gen_sound events"
        ):
            run(FakeDeluge(), "mark", "keep events gen_sound")


class ScriptEvents(unittest.TestCase):
    """--events PATTERN, shared by hardware_session.py and test_port_emu.py."""

    EVENTS = ("0: left_perform", "1: gen_mem a", "2: gen_left x", "3: gen_mem b")

    def test_none_without_the_option(self):
        self.assertIsNone(dp.events_pattern(None))
        self.assertEqual(dp.script_events(self.EVENTS, 0, [], None), [])

    def test_a_bad_pattern_says_why(self):
        with self.assertRaisesRegex(ValueError, "--events 'gen_\\(' isn't a regex"):
            dp.events_pattern("gen_(")

    def test_counts_from_the_first_start_state(self):
        pattern = dp.events_pattern("gen_mem|left")
        self.assertEqual(
            dp.script_events(self.EVENTS, 0, [1, 3], pattern),
            ["1: gen_mem a", "2: gen_left x", "3: gen_mem b"],
        )

    def test_without_a_start_state_from_where_the_script_began(self):
        pattern = dp.events_pattern("gen_mem")
        self.assertEqual(
            dp.script_events(self.EVENTS, 2, [], pattern), ["3: gen_mem b"]
        )


class ExpectEventPattern(unittest.TestCase):
    """TEXT between slashes is a regular expression, searched in each event after its timestamp."""

    def setUp(self):
        self.link = FakeDeluge(
            events={
                "button PLAY down": ["gen_ext_tempo bpm=122", "gen_ext_tempo bpm=121"]
            }
        )

    def test_matches_an_event_the_pattern_finds(self):
        failed, _, out = run(
            self.link,
            "button PLAY down",
            "expect event /gen_ext_tempo bpm=1(19|2[01])$/ within=0.1",
        )
        self.assertEqual(failed, 0, out)

    def test_fails_when_none_matches_and_prints_the_events_of_its_kind(self):
        failed, _, out = run(
            self.link,
            "button PLAY down",
            "expect event /gen_ext_tempo bpm=(119|120)$/ within=0.1",
        )
        self.assertEqual(failed, 1)
        self.assertEqual(
            out[1:],
            [
                "  got 1.0001: gen_ext_tempo bpm=122",
                "  got 1.0002: gen_ext_tempo bpm=121",
            ],
        )

    def test_the_pattern_doesnt_see_the_timestamp(self):
        failed, _, _ = run(
            self.link, "button PLAY down", "expect event /^gen_ext_tempo/ within=0.1"
        )
        self.assertEqual(failed, 0)

    def test_no_event_takes_a_pattern_too(self):
        failed, _, _ = run(
            self.link,
            "button PLAY down",
            "expect no_event /bpm=12[3-9]/ within=0.1",
        )
        self.assertEqual(failed, 0)

    def test_a_bad_pattern_is_a_script_error(self):
        with self.assertRaisesRegex(ValueError, "bad pattern"):
            run(self.link, "expect event /gen_(/")

    def test_a_pattern_with_no_kind_lists_the_events_of_its_first_word(self):
        link = FakeDeluge(events={"button BACK down": ["gen_left delete_us=300"]})
        _, _, out = run(
            link, "button BACK down", "expect event /gen_left=|gen_end/ within=0.1"
        )
        self.assertEqual(out[1:], ["  got 1.0001: gen_left delete_us=300"])

    def test_a_pattern_with_no_kind_and_none_of_its_first_word_says_what_it_looked_for(
        self,
    ):
        _, _, out = run(
            self.link,
            "button PLAY down",
            r"expect event /^\s*gen_left|gen_end/ within=0.1",
        )
        self.assertEqual(out[1:], [r"  got no /^\s*gen_left|gen_end/ event"])

    def test_a_failed_pattern_with_no_word_says_what_it_looked_for(self):
        failed, _, out = run(
            self.link, "button PLAY down", "expect event /^$/ within=0.1"
        )
        self.assertEqual(failed, 1)
        self.assertEqual(out[1:], ["  got no /^$/ event"])

    def test_the_first_word_isnt_in_a_character_class(self):
        link = FakeDeluge(events={"button BACK down": ["gen_left delete_us=300"]})
        _, _, out = run(
            link, "button BACK down", "expect event /[gG]en_left=/ within=0.1"
        )
        self.assertEqual(out[1:], ["  got 1.0001: gen_left delete_us=300"])

    def test_the_first_word_isnt_a_groups_opening(self):
        # (?P<name>...) as #288's captures write it, (?: and (?i) too: "P", "name" and "i" aren't words it looks for.
        for pattern in (
            "/(?P<ms>gen_left)=/",
            "/(?:gen_left|gen_end)=/",
            "/(?i)gen_left=/",
        ):
            with self.subTest(pattern=pattern):
                link = FakeDeluge(
                    events={"button BACK down": ["gen_left delete_us=300"]}
                )
                _, _, out = run(
                    link, "button BACK down", f"expect event {pattern} within=0.1"
                )
                self.assertEqual(out[1:], ["  got 1.0001: gen_left delete_us=300"])


class ExpectNextEvent(unittest.TestCase):
    """ "expect next event TEXT": the next event of TEXT's kind (its first word) must match TEXT. A later one that
    matches doesn't hide a wrong one before it (perform_homes.txt's gen_edit ... control=none)."""

    def setUp(self):
        self.link = FakeDeluge(
            events={
                "button PLAY down": [
                    "gen_page CLIP",
                    "gen_edit part=1 control=3",
                    "gen_edit part=1 control=none",
                ]
            }
        )

    def test_passes_when_the_next_of_its_kind_matches(self):
        failed, _, out = run(
            self.link,
            "button PLAY down",
            "expect next event gen_edit part=1 control=3 within=0.1",
            "expect next event gen_edit part=1 control=none within=0.1",
        )
        self.assertEqual(failed, 0, out)

    def test_fails_on_a_wrong_one_a_later_match_would_hide(self):
        failed, _, out = run(
            self.link,
            "button PLAY down",
            "expect next event gen_edit control=none within=0.1",
        )
        self.assertEqual(failed, 1)
        self.assertEqual(out[1], "  got 1.0002: gen_edit part=1 control=3")

    def test_takes_a_pattern_and_its_first_word_is_the_kind(self):
        failed, _, out = run(
            self.link,
            "button PLAY down",
            "expect next event /^gen_edit part=1 control=\\d+$/ within=0.1",
        )
        self.assertEqual(failed, 0, out)

    def test_fails_when_none_of_its_kind_arrives(self):
        failed, _, out = run(
            self.link, "button PLAY down", "expect next event gen_left within=0.1"
        )
        self.assertEqual(failed, 1)
        self.assertEqual(out[1:], ["  got no gen_left event"])

    def test_the_next_search_starts_after_its_match(self):
        failed, _, out = run(
            self.link,
            "button PLAY down",
            "expect next event gen_edit part=1 control=3 within=0.1",
            "expect event gen_edit part=1 control=3 within=0.1",
        )
        self.assertEqual(failed, 1, out)

    def test_needs_event_after_next(self):
        with self.assertRaisesRegex(ValueError, "bad expect"):
            run(self.link, "expect next gen_edit")

    def test_a_pattern_that_doesnt_start_with_its_kind_is_a_script_error(self):
        # Its first word isn't the event's kind: the expect would wait for "s" events and time out.
        for pattern in ("/^\\s*gen_edit/", "/gen_edit|gen_left/", "/gen_ed.t part=1/"):
            with (
                self.subTest(pattern=pattern),
                self.assertRaisesRegex(ValueError, "line 2: expect next event"),
            ):
                run(
                    self.link,
                    "button PLAY down",
                    f"expect next event {pattern} within=0.1",
                )

    def test_a_pattern_may_end_its_kind_with_the_line(self):
        failed, _, out = run(
            self.link,
            "button PLAY down",
            "expect next event /^gen_edit$/ within=0.1",
        )
        self.assertEqual(
            (failed, out[1:]), (1, ["  got 1.0002: gen_edit part=1 control=3"])
        )


class SessionEvents(unittest.TestCase):
    """deluge_probe.py session --events PATTERN prints the matching test events after each script."""

    def setUp(self):
        import argparse

        import hardware_session

        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.script = os.path.join(tmp.name, "cycle.txt")
        with open(self.script, "w") as fh:
            fh.write("button BACK down\nexpect event gen_left\n")
        self.link = FakeDeluge(
            events={
                "button BACK down": [
                    "gen_mem after_leave internal=680 external=5000",
                    "gen_press solo part=1",
                    "gen_left delete_us=300",
                ]
            }
        )
        self.lines = []
        self.session = lambda events: hardware_session.Session(
            dp,
            argparse.Namespace(
                no_load=True,
                build=None,
                args=[self.script],
                events_pattern=dp.events_pattern(events),
            ),
            hardware_session.Hardware(tmp.name),
            out=self.lines.append,
        )

    def printed(self):
        """The "  event ..." lines printed, each with the firmware's timestamp taken out (so a line without one
        doesn't match)."""
        return [
            re.sub(r"^  event \d+\.\d{4}: ", "  event ", line)
            for line in self.lines
            if line.startswith("  event")
        ]

    def test_prints_the_events_that_match(self):
        self.session("gen_mem|gen_left").run_one(dp.TestPort(self.link), self.script)
        self.assertEqual(
            self.printed(),
            [
                "  event gen_mem after_leave internal=680 external=5000",
                "  event gen_left delete_us=300",
            ],
        )

    def test_a_dot_prints_every_event(self):
        self.session(".").run_one(dp.TestPort(self.link), self.script)
        self.assertIn("  event gen_press solo part=1", self.printed())

    def test_without_the_option_no_events_are_printed(self):
        self.session(None).run_one(dp.TestPort(self.link), self.script)
        self.assertEqual(self.printed(), [])

    def test_the_events_start_state_causes_belong_to_the_previous_script(self):
        # The last script left Perform open: start_state's leave sends its gen_mem, which isn't this script's.
        self.link.events["release all"] = ["gen_mem after_leave internal=1 external=1"]
        with open(self.script, "w") as fh:
            fh.write("start_state\nbutton BACK down\nexpect event gen_left\n")
        self.session("gen_mem").run_one(dp.TestPort(self.link), self.script)
        self.assertEqual(
            self.printed(),
            ["  event gen_mem after_leave internal=680 external=5000"],
        )

    def test_a_second_start_state_doesnt_drop_the_events_before_it(self):
        with open(self.script, "w") as fh:
            fh.write("start_state\nbutton BACK down\nstart_state\n")
        self.session("gen_left").run_one(dp.TestPort(self.link), self.script)
        self.assertIn("  event gen_left delete_us=300", self.printed())

    def unsubscribing_fails(self, hang_after=None):
        """The link raises on the log-off message; after the inject `hang_after`, it answers nothing more."""
        send, hung = self.link.send, []

        def failing(msg):
            if msg == dp.LOG_OFF:
                raise OSError("the port went away")
            if not hung:
                send(msg)
            if hang_after is not None and hang_after.encode("ascii") in bytes(msg):
                hung.append(True)

        self.link.send = failing

    def test_the_events_are_printed_even_when_unsubscribing_fails(self):
        self.unsubscribing_fails()
        self.session("gen_left").run_one(dp.TestPort(self.link), self.script)
        self.assertIn("  event gen_left delete_us=300", self.printed())

    def test_a_hang_isnt_hidden_by_unsubscribing_failing(self):
        # The session tells a hang by the no-reply TimeoutError: a failed log-off mustn't replace it.
        with open(self.script, "w") as fh:
            fh.write("button BACK down\nbutton BACK up\nexpect event gen_left\n")
        self.unsubscribing_fails(hang_after="button BACK down")
        with self.assertRaises(dp.ScriptStopped) as caught:
            self.session("gen_left").run_one(dp.TestPort(self.link), self.script)
        self.assertIn("no reply to test port command", str(caught.exception.error))
        self.assertIn("  event gen_left delete_us=300", self.printed())

    def test_each_script_prints_only_its_own_events(self):
        port = dp.TestPort(self.link)
        session = self.session("gen_left")
        session.run_one(port, self.script)
        session.run_one(port, self.script)
        self.assertEqual(self.printed().count("  event gen_left delete_us=300"), 2)


class Stopped(unittest.TestCase):
    """A script an error stops (a bad line, or no reply: a hang) raises ScriptStopped with the expects it ran first
    (#283)."""

    def test_it_carries_the_error_and_how_far_the_script_got(self):
        with self.assertRaises(dp.ScriptStopped) as caught:
            dp.run_script(
                dp.TestPort(FakeDeluge()),
                ["start_state", "expect playing=false", "no such"],
                out=lambda line: None,
            )
        stop = caught.exception
        self.assertIsInstance(stop.error, ValueError)
        self.assertIs(stop.__cause__, stop.error)
        self.assertEqual(
            stop.result(),
            (1, 3, "stopped after 2 passed and 0 failed: line 3: unknown command 'no'"),
        )

    def test_a_failed_expect_before_the_stop_counts(self):
        with self.assertRaises(dp.ScriptStopped) as caught:
            dp.run_script(
                dp.TestPort(FakeDeluge()),
                ["expect playing=true within=0.1", "no such"],
                out=lambda line: None,
            )
        self.assertEqual(caught.exception.result()[:2], (2, 2))
        self.assertIn("after 0 passed and 1 failed", caught.exception.result()[2])


class RunOneScript(unittest.TestCase):
    """run_one_script(): one script file, as the hardware session and delugemu run it."""

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.script = os.path.join(tmp.name, "a.txt")
        events = [f"gen_mem n={i}" for i in range(12)]
        self.link = FakeDeluge(events={"button BACK down": events})

    def run_one(self, text):
        with open(self.script, "w") as fh:
            fh.write(text)
        out = []
        try:
            return dp.run_one_script(
                dp.TestPort(self.link), self.script, out.append
            ), out
        except dp.ScriptStopped as exc:
            return exc, out

    def test_a_stopped_script_prints_its_last_events_of_any_kind(self):
        stop, out = self.run_one(
            "button BACK down\nexpect event gen_mem n=11\nno such\n"
        )
        self.assertIsInstance(stop, dp.ScriptStopped)
        start = out.index(
            f"the last {dp.LAST_EVENTS} events before it stopped, of any kind:"
        )
        last = [re.sub(r"\d+\.\d{4}: ", "", line) for line in out[start + 1 :]]
        self.assertEqual(last, [f"last gen_mem n={i}" for i in range(2, 12)])

    def test_a_script_that_ends_prints_no_last_events(self):
        result, out = self.run_one("button BACK down\nexpect event gen_mem n=11\n")
        self.assertEqual(result, (0, 1))
        self.assertFalse([line for line in out if line.startswith("last ")], out)

    def test_a_script_that_cant_be_read_stops(self):
        out = []
        with self.assertRaises(dp.ScriptStopped) as caught:
            dp.run_one_script(
                dp.TestPort(self.link), self.script + ".missing", out.append
            )
        error = caught.exception.error
        self.assertIsInstance(error, FileNotFoundError)
        # Nothing ran: no expects, and the plain error, not "stopped after 0 passed and 0 failed" (#295).
        self.assertEqual(caught.exception.result(), (1, 0, str(error)))


class ScriptNeeds(unittest.TestCase):
    """A script's "# needs:" lines: whether delugemu can run it, and the card it needs there (#277)."""

    def needs(self, text):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "perform_x.txt")
            with open(path, "w") as fh:
                fh.write(text)
            return dp.script_needs(path), tmp

    def test_no_needs_line_runs_on_the_factory_card(self):
        needs, _ = self.needs("start_state\n")
        self.assertEqual(needs, (True, None))

    def test_a_card_line_names_a_folder_beside_the_script(self):
        needs, tmp = self.needs(
            "# needs: card fixtures/perform_x (its own card)\nstart_state\n"
        )
        self.assertEqual(needs, (True, os.path.join(tmp, "fixtures", "perform_x")))

    def test_a_second_card_line_is_refused(self):
        with self.assertRaisesRegex(ValueError, "more than one"):
            self.needs(
                "# needs: card fixtures/a\n# needs: card fixtures/b\nstart_state\n"
            )

    def test_it_takes_the_text_the_caller_already_read(self):
        path = os.path.join("dir", "perform_x.txt")  # not read: there is no such file
        needs = dp.script_needs(path, "# needs: card fixtures/x\nstart_state\n")
        self.assertEqual(needs, (True, os.path.join("dir", "fixtures", "x")))

    def test_any_other_needs_line_keeps_it_out_of_delugemu(self):
        needs, _ = self.needs(
            "# needs: card fixtures/perform_x\n# needs: a Deluge\nstart_state\n"
        )
        self.assertFalse(needs.delugemu)


class SessionCommandLine(unittest.TestCase):
    def test_events_needs_a_pattern(self):
        result = subprocess.run(
            [
                sys.executable,
                os.path.join(HERE, "deluge_probe.py"),
                "session",
                "a.txt",
                "--events",
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("--events: expected one argument", result.stderr)


if __name__ == "__main__":
    unittest.main()
