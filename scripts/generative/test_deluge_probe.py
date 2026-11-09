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
    "parse error" for the words the build doesn't know. An inject listed in `events` sends those test events, each
    after a timestamp as the firmware's TestPort::event() writes it ("%.4f: "): 1.0001, 1.0002 and so on."""

    def __init__(self, unknown=(), events=None, silent=None):
        self.unknown, self.events = set(unknown), events or {}
        self.replies, self.injected = [], []
        self.sent_events = 0
        self.silent = silent  # () -> whether state requests go unanswered now (a song load blocks the test port)

    def send(self, msg):
        if msg[:5] != dp.HDR or msg[5] != 0x02:
            return  # log subscription and the like
        command, seq, body = msg[6], msg[7], bytes(msg[8:-1]).decode("ascii")
        if command == dp.STATE:
            if self.silent is not None and self.silent():
                return
            text = json.dumps(STOPPED_IN_A_CLIP).encode("ascii")
            self.replies.append([*dp.HDR, 0x02, dp.STATE_REPLY, seq, *text, 0xF7])
            return
        word = " ".join(body.split()[:2])
        known = not any(word.startswith(w) for w in self.unknown)
        self.replies.append(
            [*dp.HDR, 0x02, dp.STATUS_REPLY, seq, 0 if known else 2, 0xF7]
        )
        if known:
            self.injected.append(body)
            for line in self.events.get(body, []):
                self.sent_events += 1
                line = f"{1 + self.sent_events / 10000:.4f}: {line}"
                self.replies.append([*dp.EVENT_HDR, *line.encode("ascii"), 0xF7])

    def send_midi(self, msg):
        pass

    def receive(self, timeout):
        return self.replies.pop(0) if self.replies else None

    def close(self):
        pass


def run(link, *lines):
    out = []
    failed, total = dp.run_script(dp.TestPort(link), list(lines), out=out.append)
    return failed, total, out


class StartState(unittest.TestCase):
    def test_a_build_in_start_state_passes(self):
        link = FakeDeluge()
        self.assertEqual(run(link, "start_state")[:2], (0, 1))
        self.assertEqual(
            link.injected,
            ["release entry", "release ramps", "release stress", "release midi_setup"],
        )

    def test_a_build_older_than_release_entry_passes_with_a_warning(self):
        failed, total, out = run(FakeDeluge(unknown=["release entry"]), "start_state")
        self.assertEqual((failed, total), (0, 1))
        self.assertTrue(
            any(
                line.startswith("WARNING line 1:") and "release entry" in line
                for line in out
            ),
            out,
        )

    def test_another_rejected_inject_still_stops_the_script(self):
        with self.assertRaises(RuntimeError):
            run(
                FakeDeluge(unknown=["button SHIFT"]), "start_state", "button SHIFT down"
            )


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
        self.assertEqual(link.injected, ["load song SONGS/MY SONG.XML"])

    def test_a_report_input_line_is_queued_on_the_firmware(self):
        link = FakeDeluge(
            events={
                "report input": ["input_lag ok=yes max_ms=3 presses=2 gold_gap_ms=0"]
            }
        )
        self.assertEqual(
            run(link, "report input", "expect event input_lag ok=yes")[:2], (0, 1)
        )
        self.assertEqual(link.injected, ["report input"])


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
        self.assertEqual(out[2:], ["  got 1.0003: gen_left delete_us=310"])

    def test_says_when_none_of_that_kind_arrived(self):
        _, _, out = run(
            self.link, "button BACK down", "expect event gen_mem within=0.1"
        )
        self.assertEqual(out[1:], ["  got no gen_mem event"])

    def test_a_pass_prints_nothing_more(self):
        _, _, out = run(self.link, "button BACK down", "expect event gen_left")
        self.assertEqual(out, ["PASS line 2: expect event gen_left"])


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
                no_load=True, build=None, args=[self.script], events=events
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
        self.link.events["release entry"] = [
            "gen_mem after_leave internal=1 external=1"
        ]
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
        with self.assertRaises(TimeoutError) as caught:
            self.session("gen_left").run_one(dp.TestPort(self.link), self.script)
        self.assertIn("no reply to test port command", str(caught.exception))
        self.assertIn("  event gen_left delete_us=300", self.printed())

    def test_each_script_prints_only_its_own_events(self):
        port = dp.TestPort(self.link)
        session = self.session("gen_left")
        session.run_one(port, self.script)
        session.run_one(port, self.script)
        self.assertEqual(self.printed().count("  event gen_left delete_us=300"), 2)


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
