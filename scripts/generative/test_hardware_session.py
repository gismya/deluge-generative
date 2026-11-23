"""Tests for hardware_session.py (deluge_probe.py session) without a Deluge: the state files and the run's rules.

python3 scripts/generative/test_hardware_session.py
"""

import argparse
import os
import re
import socket
import sys
import tempfile
import time
import unittest
from typing import ClassVar
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import deluge_files
import deluge_probe
import hardware_session as hs


def args(**kw):
    base = {
        "args": ["a.txt"],
        "build": None,
        "no_load": True,
        "wait": 0,
        "no_wait": True,
        "owner": None,
        "note": None,
        "timeout": 1,
        "udp": None,
        "clear_hung": False,
        "events": None,
        "key": None,
        "key_file": None,
        "force_build_mismatch": False,
    }
    base.update(kw)
    return argparse.Namespace(**base)


class FakeLink:
    def close(self):
        pass


class FakePort:
    link = FakeLink()


HEAD = "abc123def4567890abc123def4567890abc123de"  # the checkout's HEAD in FakeSession; Base loads its build


class FakeSession(hs.Session):
    """The device steps scripted: ready gives a port or None, each script returns or raises. `listings` are the
    SONGS/ and RECIPES/ files card_files() returns, one set a call (none: empty), or an exception it raises; `writes`
    maps a script to its "# writes:" globs."""

    def __init__(self, hw, ready=True, scripts=None, listings=None, writes=None, **kw):
        a = args(**kw)
        super().__init__(deluge_probe, a, hw, out=self.lines.append)
        self.ready, self.results, self.loads = ready, scripts or {}, 0
        self.listings, self.writes = list(listings or []), writes or {}

    lines: ClassVar[list]

    def checkout_sha(self):
        return HEAD

    def card_files(self, port):
        listing = self.listings.pop(0) if self.listings else set()
        if isinstance(listing, Exception):
            raise listing
        return listing

    def script_writes(self, script):
        return self.writes.get(script, [])

    def wait_ready(self, fresh, before=None):
        return FakePort() if self.ready else None

    def run_one(self, port, script):
        result = self.results.get(script, (0, 3))
        if isinstance(result, Exception):
            raise result
        return result


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.hw = hs.Hardware(self.tmp.name)
        # Every session runs on the build of its checkout's HEAD, with nothing in SONGS/ or RECIPES/, unless a test
        # says otherwise.
        self.hw.write_loaded({"build": "hardware", "sha": HEAD[:9], "when": "earlier"})
        for name, value in (("checkout_sha", HEAD), ("card_files", set())):
            patcher = mock.patch.object(hs.Session, name, lambda *a, v=value: v)
            patcher.start()
            self.addCleanup(patcher.stop)
        FakeSession.lines = []

    def tearDown(self):
        self.tmp.cleanup()

    def released(self):
        return (
            sorted(os.listdir(self.hw.released))
            if os.path.isdir(self.hw.released)
            else []
        )


class Lock(Base):
    def test_taken_then_moved_into_released(self):
        self.assertTrue(self.hw.acquire("x", 0, out=lambda m: None))
        self.assertTrue(os.path.isdir(self.hw.lock))
        self.hw.release()
        self.assertFalse(os.path.exists(self.hw.lock))
        self.assertEqual(len(self.released()), 1)

    def test_held_by_someone_else_fails_without_waiting(self):
        os.makedirs(self.hw.lock)
        with open(os.path.join(self.hw.lock, "owner"), "w") as fh:
            fh.write("pid=1\nhost=elsewhere\n")
        self.assertFalse(self.hw.acquire("x", 0, out=lambda m: None))
        self.hw.release()  # not ours: left alone
        self.assertTrue(os.path.isdir(self.hw.lock))

    def test_an_ownerless_lock_over_a_minute_old_is_stale(self):
        os.makedirs(self.hw.lock)
        made = os.stat(self.hw.lock).st_mtime
        self.assertFalse(self.hw._stale(now=made + 30))
        self.assertTrue(self.hw._stale(now=made + hs.OWNERLESS_STALE_SECONDS + 1))
        old = made - hs.OWNERLESS_STALE_SECONDS - 1
        os.utime(self.hw.lock, (old, old))
        self.assertTrue(self.hw.acquire("x", 0, out=lambda m: None))
        self.assertTrue(any("stale-lock" in n for n in self.released()))

    def test_a_fresh_ownerless_lock_is_held(self):
        os.makedirs(self.hw.lock)
        self.assertFalse(self.hw.acquire("x", 0, out=lambda m: None))

    def test_a_dead_owner_on_this_host_is_stale(self):
        self.write_lock("pid=999999")
        self.assertTrue(self.hw.acquire("x", 0, out=lambda m: None))
        self.assertTrue(any("stale-lock" in n for n in self.released()))

    def write_lock(self, pid_line, path=None):
        os.makedirs(path or self.hw.lock)
        with open(os.path.join(path or self.hw.lock, "owner"), "w") as fh:
            fh.write(f"{pid_line}\nhost={socket.gethostname()}\n")

    def racing(self, then, while_taking=False):
        """A Hardware where another session acts (`then`) right after this one finds the lock stale: the first time,
        or with while_taking, once it holds lock.take."""
        hw, judge = self.hw, self.hw._stale

        def stale(now=None):
            found = judge(now)
            if found and os.path.exists(hw.take_lock) == while_taking:
                then()
            return found

        hw._stale = stale
        return hw

    def test_a_stale_lock_another_session_moved_first_is_taken_afresh(self):
        self.write_lock("pid=999999")
        hw = self.racing(
            lambda: os.rename(self.hw.lock, os.path.join(self.tmp.name, "theirs"))
        )
        self.assertTrue(hw.acquire("x", 0, out=lambda m: None))
        self.assertIn("pid=", hw.owner_text())

    def replace_with_a_live_lock(self):
        os.rename(self.hw.lock, os.path.join(self.tmp.name, "theirs"))
        self.write_lock(f"pid={os.getppid()}\nnote=live")

    def test_a_live_lock_that_replaced_the_stale_one_is_left_alone(self):
        self.write_lock("pid=999999")
        hw = self.racing(self.replace_with_a_live_lock)
        self.assertFalse(hw.acquire("x", 0, out=lambda m: None))
        self.assertIn("note=live", hw.owner_text())
        self.assertFalse(any("stale-lock" in n for n in self.released()))

    def test_a_live_lock_moved_by_mistake_is_put_back(self):
        # Replaced between the judging under lock.take and the move: the inode tells.
        self.write_lock("pid=999999")
        hw = self.racing(self.replace_with_a_live_lock, while_taking=True)
        self.assertFalse(hw.acquire("x", 0, out=lambda m: None))
        self.assertIn("note=live", hw.owner_text())
        self.assertFalse(any("stale-lock" in n for n in self.released()))

    def test_only_one_session_at_a_time_takes_a_stale_lock(self):
        # B finds the same stale lock while A is judging it: B leaves it to A, so B can't move the lock A takes next.
        self.write_lock("pid=999999")
        other = hs.Hardware(self.tmp.name)
        tried = []
        hw = self.racing(
            lambda: tried.append(other._take_stale(lambda m: None)), while_taking=True
        )
        self.assertTrue(hw.acquire("x", 0, out=lambda m: None))
        self.assertEqual(tried, [False])
        self.assertEqual(sum("stale-lock" in n for n in self.released()), 1)
        self.assertFalse(os.path.exists(hw.take_lock))

    def test_a_stale_lock_is_left_while_another_session_is_taking_it(self):
        self.write_lock("pid=999999")
        os.makedirs(self.hw.take_lock)
        self.assertFalse(self.hw.acquire("x", 0, out=lambda m: None))
        self.assertIn("pid=999999", self.hw.owner_text())

    def test_a_take_left_by_a_session_that_died_over_a_minute_ago_is_moved(self):
        self.write_lock("pid=999999")
        os.makedirs(self.hw.take_lock)
        old = time.time() - hs.OWNERLESS_STALE_SECONDS - 1
        os.utime(self.hw.take_lock, (old, old))
        self.assertTrue(self.hw.acquire("x", 0, out=lambda m: None))
        self.assertTrue(any("stale-take" in n for n in self.released()))

    def test_a_live_take_lock_that_replaced_a_dead_one_is_put_back(self):
        # Another session moved the dead take_lock and made its own just before this one's move.
        self.write_lock("pid=999999")
        os.makedirs(self.hw.take_lock)
        old = time.time() - hs.OWNERLESS_STALE_SECONDS - 1
        os.utime(self.hw.take_lock, (old, old))
        move, theirs = self.hw._move_to_released, []

        def replaced_first(path, label):
            if label == "stale-take" and not theirs:
                os.rename(path, os.path.join(self.tmp.name, "dead"))
                os.mkdir(path)
                theirs.append(os.stat(path).st_ino)
            return move(path, label)

        self.hw._move_to_released = replaced_first
        self.assertFalse(self.hw.acquire("x", 0, out=lambda m: None))
        self.assertEqual(os.stat(self.hw.take_lock).st_ino, theirs[0])
        self.assertIn("pid=999999", self.hw.owner_text())

    def test_a_take_lock_another_session_took_over_is_left_to_it(self):
        # This session's lock.take was moved as dead while it held it, and another session made its own.
        self.write_lock("pid=999999")

        def take_over():
            os.rename(self.hw.take_lock, os.path.join(self.tmp.name, "ours"))
            os.mkdir(self.hw.take_lock)

        hw = self.racing(take_over, while_taking=True)
        hw.acquire("x", 0, out=lambda m: None)
        self.assertTrue(os.path.isdir(hw.take_lock))

    def test_a_take_lock_moved_away_while_held_is_no_error(self):
        self.write_lock("pid=999999")
        hw = self.racing(
            lambda: os.rename(self.hw.take_lock, os.path.join(self.tmp.name, "ours")),
            while_taking=True,
        )
        self.assertTrue(hw.acquire("x", 0, out=lambda m: None))
        self.assertFalse(os.path.exists(hw.take_lock))

    def test_a_lock_moved_away_before_its_owner_is_written_is_taken_again(self):
        mkdir, lost = os.mkdir, []

        def mkdir_then_lose(path, *a):
            mkdir(path, *a)
            if path == self.hw.lock and not lost:
                lost.append(path)
                os.rename(path, os.path.join(self.tmp.name, "moved"))

        with mock.patch.object(hs.os, "mkdir", mkdir_then_lose):
            self.assertTrue(self.hw.acquire("x", 0, out=lambda m: None))
        self.assertEqual(lost, [self.hw.lock])
        self.assertIn(f"pid={os.getpid()}", self.hw.owner_text())


class Pending(Base):
    def test_the_same_run_is_added_once(self):
        self.assertTrue(
            self.hw.add_pending(
                "why", "relwithdebinfo", ["a.txt"], "and the OLED shows X"
            )
        )
        self.assertFalse(self.hw.add_pending("why", "relwithdebinfo", ["a.txt"]))
        with open(self.hw.pending) as fh:
            text = fh.read()
        self.assertIn("--build relwithdebinfo a.txt", text)
        self.assertIn("N expects, 0 failed", text)
        self.assertIn("and the OLED shows X", text)


class RamLoadedNote(unittest.TestCase):
    record: ClassVar[dict] = {
        "build": "relwithdebinfo",
        "sha": "abc",
        "when": "then",
        "loaded_at": 1000.0,
    }

    def test_still_up_since_the_load_is_a_ram_loaded_build(self):
        self.assertIn(
            "loading over a RAM-loaded build",
            hs.ram_loaded_note(self.record, 500, now=1400),
        )

    def test_rebooted_since_the_load_says_nothing(self):
        self.assertEqual(hs.ram_loaded_note(self.record, 30, now=5000), "")

    def test_no_record_says_nothing(self):
        self.assertEqual(hs.ram_loaded_note(None, 30), "")


class Rules(Base):
    def test_refuses_while_hung_and_adds_the_run_to_pending(self):
        self.hw.write_hung("froze", None, ["a.txt"])
        s = FakeSession(self.hw)
        self.assertEqual(s.run(), hs.EXIT_REFUSED)
        self.assertTrue(os.path.exists(self.hw.pending))
        self.assertFalse(os.path.exists(self.hw.lock))
        self.assertTrue(any("delugemu" in line for line in s.lines))

    def test_no_ready_writes_the_hung_marker_and_releases_the_lock(self):
        s = FakeSession(self.hw, ready=False)
        self.assertEqual(s.run(), hs.EXIT_HUNG)
        self.assertIn("no answer from the test port", self.hw.hung_text())
        self.assertFalse(os.path.exists(self.hw.lock))

    def test_no_reply_in_a_script_writes_the_hung_marker_and_stops(self):
        scripts = {
            "a.txt": deluge_probe.ScriptStopped(
                TimeoutError("no reply to test port command 0x10")
            )
        }
        s = FakeSession(self.hw, scripts=scripts, args=["a.txt", "b.txt"])
        self.assertEqual(s.run(), hs.EXIT_HUNG)
        self.assertIn("during a.txt", self.hw.hung_text())
        self.assertFalse(any("b.txt:" in line for line in s.lines))

    def test_a_script_error_counts_as_a_failed_expect_and_the_next_script_runs(self):
        scripts = {"a.txt": deluge_probe.ScriptStopped(ValueError("line 2: bad"), 0, 1)}
        s = FakeSession(self.hw, scripts=scripts, args=["a.txt", "b.txt"])
        self.assertEqual(s.run(), hs.EXIT_FAILED)
        self.assertIn(
            "  a.txt: 2 expects, 1 failed (stopped after 1 passed and 0 failed: line 2: bad)",
            s.lines,
        )
        self.assertIn("  b.txt: 3 expects, 0 failed", s.lines)
        self.assertIsNone(self.hw.hung_text())

    def test_the_delugemu_runs_it_suggests_go_one_card_each(self):
        # test_port_emu.py builds a script's card from its "# needs: card DIR" line, so scripts on different
        # cards go in different runs (#294). A script that isn't there runs on the factory card.
        plain, card = (
            os.path.join(self.tmp.name, n) for n in ("plain.txt", "card.txt")
        )
        for path, text in ((plain, ""), (card, "# needs: card fixtures/x\n")):
            with open(path, "w") as fh:
                fh.write(text + "start_state\n")
        run = "python3 scripts/generative/test_port_emu.py --build --out <scratch dir> "
        self.assertEqual(
            hs.emu_commands(deluge_probe, [plain, card, "missing.txt"]),
            [
                f"{run}--script {plain} --script missing.txt",
                f"{run}--script {card}",
            ],
        )

    def test_counts_every_script_and_fails_on_any_failure(self):
        s = FakeSession(
            self.hw, scripts={"a.txt": (0, 5), "b.txt": (2, 7)}, args=["a.txt", "b.txt"]
        )
        self.assertEqual(s.run(), hs.EXIT_FAILED)
        self.assertIn("  a.txt: 5 expects, 0 failed", s.lines)
        self.assertIn("  b.txt: 7 expects, 2 failed", s.lines)
        self.assertIsNone(self.hw.hung_text())
        self.assertFalse(os.path.exists(self.hw.lock))

    def test_an_unexpected_error_still_releases_the_lock(self):
        s = FakeSession(self.hw, scripts={"a.txt": KeyError("boom")})
        with self.assertRaises(KeyError):
            s.run()
        self.assertFalse(os.path.exists(self.hw.lock))


class NoLoadBuild(Base):
    """--no-load runs only on the build this checkout loaded (#464)."""

    def test_the_build_of_head_runs(self):
        self.assertEqual(FakeSession(self.hw).run(), 0)

    def test_a_dirty_build_of_head_matches_head(self):
        self.hw.write_loaded({"build": "hardware", "sha": HEAD[:9] + "-dirty"})
        self.assertEqual(FakeSession(self.hw).run(), 0)

    def test_another_build_refuses_naming_both_shas_before_the_lock(self):
        self.hw.write_loaded({"build": "hardware", "sha": "fff000111", "when": "then"})
        s = FakeSession(self.hw)
        self.assertEqual(s.run(), hs.EXIT_FAILED)
        refusal = next(line for line in s.lines if "fff000111" in line)
        self.assertIn(HEAD[:9], refusal)
        self.assertTrue(any("--force-build-mismatch" in line for line in s.lines))
        self.assertEqual(self.released(), [])  # the lock was never taken
        self.assertFalse(any("a.txt" in line for line in s.lines))

    def test_no_record_of_a_load_refuses(self):
        os.remove(self.hw.loaded)
        s = FakeSession(self.hw)
        self.assertEqual(s.run(), hs.EXIT_FAILED)
        self.assertTrue(any(self.hw.loaded in line for line in s.lines))

    def test_a_load_while_it_waited_for_the_lock_refuses(self):
        acquire = self.hw.acquire

        def loaded_meanwhile(*a, **kw):
            self.hw.write_loaded({"build": "hardware", "sha": "fff000111"})
            return acquire(*a, **kw)

        self.hw.acquire = loaded_meanwhile
        s = FakeSession(self.hw)
        self.assertEqual(s.run(), hs.EXIT_FAILED)
        self.assertTrue(any("fff000111" in line for line in s.lines))
        self.assertFalse(any("a.txt:" in line for line in s.lines))
        self.assertFalse(os.path.exists(self.hw.lock))

    def test_the_force_flag_warns_and_runs(self):
        self.hw.write_loaded({"build": "hardware", "sha": "fff000111"})
        s = FakeSession(self.hw, force_build_mismatch=True)
        self.assertEqual(s.run(), 0)
        warning = next(line for line in s.lines if "fff000111" in line)
        self.assertIn("warning", warning)
        self.assertIn("  a.txt: 3 expects, 0 failed", s.lines)

    def test_delugemu_isnt_checked(self):
        os.remove(self.hw.loaded)
        self.assertEqual(FakeSession(self.hw, udp="1:2").run(), 0)

    def test_matching_takes_a_short_sha_as_a_prefix_of_head(self):
        self.assertIsNone(hs.build_mismatch({"sha": HEAD[:9]}, HEAD, "loaded.json"))
        self.assertIsNotNone(hs.build_mismatch({"sha": "unknown"}, HEAD, "loaded.json"))
        self.assertIsNotNone(hs.build_mismatch({"sha": "-dirty"}, HEAD, "loaded.json"))


class CardWrites(Base):
    """New files in SONGS/ and RECIPES/ after a session fail it unless a script declares them (#465)."""

    NEW = "SONGS/GEN ACID TECHNO 3E9D8DZ LIVE.XML"

    def test_a_declared_new_file_passes_and_is_named(self):
        s = FakeSession(
            self.hw,
            listings=[{"SONGS/Ttt.XML"}, {"SONGS/Ttt.XML", self.NEW}],
            writes={"a.txt": ["SONGS/GEN ACID TECHNO *"]},
        )
        self.assertEqual(s.run(), 0)
        self.assertTrue(any(self.NEW in line for line in s.lines))

    def test_an_undeclared_new_file_fails_and_is_named(self):
        s = FakeSession(self.hw, listings=[set(), {"SONGS/SONG001.XML"}])
        self.assertEqual(s.run(), hs.EXIT_FAILED)
        failure = next(line for line in s.lines if "SONG001" in line)
        self.assertIn("undeclared", failure)
        self.assertIn("  a.txt: 3 expects, 0 failed", s.lines)

    def test_another_scripts_declaration_covers_it(self):
        s = FakeSession(
            self.hw,
            args=["a.txt", "b.txt"],
            listings=[set(), {"RECIPES/ACID TECHNO 1234567.XML"}],
            writes={"b.txt": ["RECIPES/ACID TECHNO *"]},
        )
        self.assertEqual(s.run(), 0)

    def test_an_unreadable_script_still_lets_the_others_be_checked(self):
        class Unreadable(FakeSession):
            def script_writes(self, script):
                if script == "a.txt":
                    raise ValueError("include missing.txt: not found")
                return super().script_writes(script)

        s = Unreadable(
            self.hw,
            args=["a.txt", "b.txt"],
            listings=[set(), {"RECIPES/ACID TECHNO 1234567.XML", "SONGS/X.XML"}],
            writes={"b.txt": ["/RECIPES/ACID TECHNO *"]},
        )
        self.assertEqual(s.run(), hs.EXIT_FAILED)
        self.assertTrue(
            any(
                "declared: /RECIPES/ACID TECHNO 1234567.XML" in line for line in s.lines
            )
        )
        self.assertTrue(any("undeclared: /SONGS/X.XML" in line for line in s.lines))

    def test_a_failed_script_still_gets_the_check(self):
        s = FakeSession(
            self.hw, scripts={"a.txt": (1, 3)}, listings=[set(), {"SONGS/X.XML"}]
        )
        self.assertEqual(s.run(), hs.EXIT_FAILED)
        self.assertTrue(any("SONGS/X.XML" in line for line in s.lines))

    def test_no_reply_to_the_listing_fails_without_a_hung_marker(self):
        s = FakeSession(self.hw, listings=[TimeoutError("no reply to dir within 10 s")])
        self.assertEqual(s.run(), hs.EXIT_FAILED)
        self.assertIsNone(self.hw.hung_text())
        self.assertTrue(any("no reply to dir" in line for line in s.lines))
        self.assertFalse(any("a.txt:" in line for line in s.lines))

    def test_a_listing_error_fails_before_any_script(self):
        error = deluge_files.CardError(
            "ls", "/SONGS", deluge_files.FRESULT.index("FR_DISK_ERR")
        )
        s = FakeSession(self.hw, listings=[error])
        self.assertEqual(s.run(), hs.EXIT_FAILED)
        self.assertFalse(any("a.txt:" in line for line in s.lines))
        self.assertIsNone(self.hw.hung_text())


class StopsReplying(Base):
    """A device that hangs partway through a script (#279's perform_retire.txt hang): the session reports how far the
    script got, the last events of any kind before the hang, and when each expect passed."""

    SCRIPT = (
        "start_state\n"
        "button BACK down\n"
        "expect event gen_left\n"
        "expect playing=false\n"
        "button BACK up\n"
        "expect event gen_end\n"
    )

    def setUp(self):
        super().setUp()
        from test_deluge_probe import FakeClock, FakeDeluge

        # Virtual time for the probe: a state request with no reply is retried for IDLE_TIMEOUT.
        patcher = mock.patch.object(deluge_probe, "time", FakeClock())
        patcher.start()
        self.addCleanup(patcher.stop)

        self.script = os.path.join(self.tmp.name, "a.txt")
        with open(self.script, "w") as fh:
            fh.write(self.SCRIPT)
        events = [f"gen_mem n={i}" for i in range(12)] + ["gen_left delete_us=300"]
        link = FakeDeluge(events={"button BACK down": events})
        hung, send = [], link.send

        def silent_after_back_up(msg):
            if not hung:
                send(msg)
            if b"button BACK up" in bytes(msg):
                hung.append(True)

        link.send = silent_after_back_up
        port = deluge_probe.TestPort(link)
        a = args(args=[self.script])
        a.events_pattern = deluge_probe.events_pattern("gen_left")
        self.lines = []

        class Session(hs.Session):
            def wait_ready(self, fresh, before=None):
                return port

        self.session = Session(deluge_probe, a, self.hw, out=self.lines.append)

    def test_it_says_how_far_the_script_got(self):
        self.assertEqual(self.session.run(), hs.EXIT_HUNG)
        self.assertIn(
            "  a.txt: 4 expects, 1 failed (stopped after 3 passed and 0 failed: no reply to test port command 0x11)",
            self.lines,
        )

    def test_it_prints_the_last_events_of_any_kind(self):
        self.session.run()
        start = self.lines.index("  the last 10 events before it stopped, of any kind:")
        last = [
            re.sub(r"\d+\.\d{4}: ", "", line)
            for line in self.lines[start + 1 : start + 11]
        ]
        self.assertEqual(
            last,
            [f"  last gen_mem n={i}" for i in range(3, 12)]
            + ["  last gen_left delete_us=300"],
        )

    def test_each_pass_line_has_the_time_it_passed(self):
        self.session.run()
        passes = [line for line in self.lines if " PASS line" in line]
        self.assertEqual(len(passes), 3, self.lines)
        for line in passes:
            self.assertRegex(line, r"^  \d\d:\d\d:\d\d\.\d{3} PASS line \d: ")


class AnswersOnceProbe:
    """deluge_probe as seen by Session.wait_ready: the device answers wait_ready() once, then its state() raises."""

    link_failed = staticmethod(deluge_probe.link_failed)
    read_key = staticmethod(deluge_probe.read_key)
    script_needs = staticmethod(deluge_probe.script_needs)

    def __init__(self, error):
        self.error, self.closed = error, 0

    def wait_ready(self, open_link, key, timeout):
        probe = self

        class Link:
            def close(self):
                probe.closed += 1

        class Port:
            link = Link()

            def state(self):
                raise probe.error

        return Port()


class UptimesProbe:
    """deluge_probe as seen by Session.wait_ready: each wait_ready() gives a port whose state() reads the next
    uptime, in seconds."""

    link_failed = staticmethod(deluge_probe.link_failed)
    read_key = staticmethod(deluge_probe.read_key)
    script_needs = staticmethod(deluge_probe.script_needs)

    def __init__(self, uptimes):
        self.uptimes = list(uptimes)

    def wait_ready(self, open_link, key, timeout):
        seconds = self.uptimes.pop(0)

        class Port:
            link = FakeLink()
            uptime = seconds

            def state(self):
                return {"sample_timer": int(seconds * hs.SAMPLE_RATE)}

        return Port()


class FakeClock:
    """hardware_session's time module with a clock that only sleep() moves: a wait of seconds takes none. time()
    is the real wall clock (the lock's age and the loaded record)."""

    def __init__(self):
        self.now = 1000.0

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        self.now += max(0.0, seconds)

    time = staticmethod(time.time)


class FakeClockBase(Base):
    """Base with hs.time faked (FakeClock): the waits on the test port take no real time."""

    def setUp(self):
        super().setUp()
        self.clock = FakeClock()
        patcher = mock.patch.object(hs, "time", self.clock)
        patcher.start()
        self.addCleanup(patcher.stop)


class FastPoll(FakeClockBase):
    """Polls the test port every 0.05 s instead of every READY_POLL_SECONDS, so a wait of a second or two holds
    enough answers to tell one near its end from an early one."""

    def setUp(self):
        super().setUp()
        patcher = mock.patch.object(hs, "READY_POLL_SECONDS", 0.05)
        patcher.start()
        self.addCleanup(patcher.stop)


class FreshBoot(FastPoll):
    def ready_after_load(self, uptimes, before):
        s = hs.Session(
            UptimesProbe(uptimes), args(timeout=5), self.hw, out=lambda m: None
        )
        return s.wait_ready(fresh=True, before=before)

    def test_the_old_build_still_answering_soon_after_its_own_boot_isnt_the_new_one(
        self,
    ):
        # The old build had been up 12 s; a load that failed its checksum leaves it running at 21 s.
        port = self.ready_after_load([21, 4], before=(12.0, self.clock.monotonic() - 9))
        self.assertEqual(port.uptime, 4)

    def test_a_new_build_answering_after_a_device_that_had_just_booted_is_fresh(self):
        # Power cycled, then loaded 8 s after boot: the new build answers at 6 s, below the old one's 8 + 20.
        port = self.ready_after_load([6], before=(8.0, self.clock.monotonic() - 20))
        self.assertEqual(port.uptime, 6)

    def test_with_no_uptime_from_before_the_load_any_young_build_is_fresh(self):
        port = self.ready_after_load([25], before=None)
        self.assertEqual(port.uptime, 25)

    def test_a_new_build_that_first_answers_after_a_slow_boot_is_fresh(self):
        # Up 10 min before the load; the new build loads its boot song and first answers at 45 s.
        now = 1000.0
        self.assertTrue(hs.fresh_boot(45 * hs.SAMPLE_RATE, (600.0, now - 60), now=now))

    def test_an_old_build_whose_timer_fell_behind_isnt_the_new_one(self):
        # Up 600 s, read 60 s ago: its timer lost 3 s meanwhile (the transfer), so it reads 657 s, not 660.
        now = 1000.0
        self.assertFalse(
            hs.fresh_boot(657 * hs.SAMPLE_RATE, (600.0, now - 60), now=now)
        )

    def test_with_no_uptime_from_before_the_load_an_old_uptime_isnt_fresh(self):
        self.assertFalse(hs.fresh_boot(45 * hs.SAMPLE_RATE, None))

    def test_an_old_build_whose_timer_just_wrapped_isnt_the_new_one(self):
        # Up 27 h: its 32-bit sample timer wrapped 5 s ago and reads 5 s, as a new build would.
        now = 1000.0
        before = (hs.TIMER_WRAP_SECONDS - 5, now - 10)
        self.assertFalse(hs.fresh_boot(5 * hs.SAMPLE_RATE, before, now=now))

    def test_an_old_build_answering_to_the_end_is_a_load_that_didnt_take(self):
        session = hs.Session(
            UptimesProbe([40] * 1000), args(timeout=1), self.hw, out=lambda m: None
        )
        with self.assertRaises(hs.LoadDidNotTake) as caught:
            session.wait_ready(fresh=True)
        self.assertEqual(caught.exception.uptime, 40)


class OldThenSilentProbe(UptimesProbe):
    """After `start_after` seconds of silence, the old build answers each wait_ready() with its uptime (None: a port
    whose state() fails), then the device goes silent: like deluge_probe.wait_ready(), a call waits out its timeout
    (or start_after, if that's shorter) and raises. It waits on hs.time, the FakeClock in a FakeClockBase test."""

    def __init__(self, uptimes, start_after=0.0):
        super().__init__(uptimes)
        self.start_after = start_after

    def wait_ready(self, open_link, key, timeout):
        if self.start_after:
            hs.time.sleep(min(self.start_after, timeout))
            self.start_after = 0.0
        if not self.uptimes:
            hs.time.sleep(timeout)
            raise TimeoutError("the test port did not answer")
        if self.uptimes[0] is None:
            self.uptimes.pop(0)

            class Port:
                link = FakeLink()

                def state(self):
                    raise TimeoutError("no reply to test port command 0x11")

            return Port()
        return super().wait_ready(open_link, key, timeout)


class HardwareBuild(Base):
    """--build hardware: the session builds build-hardware/ (Release, both options) itself, so build/ keeps the
    default configuration for check_all.py and ./dbt sizediff."""

    def test_it_is_built_before_the_lock_is_taken(self):
        held = []

        class Building(FakeSession):
            def build_firmware(self):
                held.append(os.path.exists(self.hw.lock))
                return 0

        s = Building(self.hw, build="hardware", no_load=False)
        with mock.patch.object(hs, "check_build_config", return_value="stop here"):
            self.assertEqual(s.run(), hs.EXIT_FAILED)
        self.assertEqual(held, [False])
        self.assertIn("session: stop here", s.lines)

    def test_a_failed_build_stops_the_run_without_a_marker(self):
        class Failing(FakeSession):
            def build_firmware(self):
                return 1

        s = Failing(self.hw, build="hardware", no_load=False)
        self.assertEqual(s.run(), hs.EXIT_FAILED)
        self.assertIsNone(self.hw.hung_text())
        self.assertFalse(os.path.exists(self.hw.lock))

    def test_other_builds_are_not_built(self):
        class Counting(FakeSession):
            def build_firmware(self):
                raise AssertionError("built")

        s = Counting(self.hw, build="release", no_load=False)
        with mock.patch.object(hs, "check_build_config", return_value="stop here"):
            self.assertEqual(s.run(), hs.EXIT_FAILED)

    def test_its_config_and_binary_are_in_build_hardware(self):
        with mock.patch.object(hs, "REPO", self.tmp.name):
            self.assertIn("build-hardware", hs.check_build_config("hardware"))
            os.makedirs(os.path.join(self.tmp.name, "build-hardware", "Release"))
            with open(
                os.path.join(self.tmp.name, "build-hardware", "CMakeCache.txt"), "w"
            ) as fh:
                fh.write("ENABLE_SYSEX_TEST:BOOL=ON\nENABLE_SYSEX_LOAD:BOOL=ON\n")
            open(
                os.path.join(self.tmp.name, "build-hardware", "Release", "deluge.bin"),
                "wb",
            ).close()
            self.assertIsNone(hs.check_build_config("hardware"))
            self.assertEqual(
                hs.binary_for("hardware"),
                os.path.join(self.tmp.name, "build-hardware", "Release", "deluge.bin"),
            )


class ConfigureAndBuild(Base):
    CONFIGURED = "CMAKE_BUILD_TYPE:STRING=\nENABLE_SYSEX_TEST:BOOL=ON\n"

    def run_it(self, cache=None, ninja=True):
        """configure_and_build() on a build folder with `cache` as its CMakeCache.txt (None: none) and a build.ninja
        if `ninja`. Returns the commands given to ./dbt exec."""
        root = os.path.join(self.tmp.name, f"build{len(os.listdir(self.tmp.name))}")
        os.makedirs(root)
        if cache is not None:
            with open(os.path.join(root, "CMakeCache.txt"), "w") as f:
                f.write(cache)
        if ninja:
            open(os.path.join(root, "build.ninja"), "w").close()
        with mock.patch.object(hs.subprocess, "run") as run:
            run.return_value.returncode = 0
            self.assertEqual(
                hs.configure_and_build(root, ["-DENABLE_SYSEX_TEST=ON"]), 0
            )
        return [c.args[0][2] for c in run.call_args_list]

    def test_configures_with_the_options_only_the_first_time(self):
        first = self.run_it(cache=None, ninja=False)
        self.assertEqual(len(first), 2)
        self.assertIn("-DENABLE_SYSEX_TEST=ON", first[0])
        self.assertTrue(first[1].startswith("cmake --build"), first)
        self.assertEqual(len(self.run_it(cache=self.CONFIGURED)), 1)

    def test_configures_again_when_a_configure_stopped_before_build_ninja(self):
        commands = self.run_it(cache=self.CONFIGURED, ninja=False)
        self.assertEqual(len(commands), 2)
        self.assertIn("-DENABLE_SYSEX_TEST=ON", commands[0])

    def test_configures_again_when_the_cache_lacks_an_option(self):
        for cache in ("CMAKE_BUILD_TYPE:STRING=\n", "ENABLE_SYSEX_TEST:BOOL=OFF\n"):
            with self.subTest(cache=cache):
                commands = self.run_it(cache=cache)
                self.assertEqual(len(commands), 2)
                self.assertIn("-DENABLE_SYSEX_TEST=ON", commands[0])


class MainArgs(Base):
    def test_a_load_needs_a_timeout_of_at_least_four_polls(self):
        script = os.path.join(self.tmp.name, "a.txt")
        open(script, "w").close()
        lines = []
        code = hs.main(
            deluge_probe,
            args(no_load=False, build="release", timeout=1, args=[script]),
            out=lines.append,
        )
        self.assertEqual(code, hs.EXIT_FAILED)
        self.assertTrue(any("--timeout" in line for line in lines), lines)

    def test_the_force_flag_goes_with_no_load(self):
        lines = []
        code = hs.main(
            deluge_probe,
            args(no_load=False, build="release", force_build_mismatch=True),
            out=lines.append,
        )
        self.assertEqual(code, hs.EXIT_FAILED)
        self.assertTrue(any("--force-build-mismatch" in line for line in lines), lines)


class FailedLoad(FastPoll):
    """A load that loadfw reports as done but that didn't take: the old build still runs."""

    def setUp(self):
        super().setUp()
        os.remove(self.hw.loaded)  # no load before this one: no RAM-loaded note

    def run_load(self, probe, uptime_before=600.0, timeout=1, still_answers=None):
        """Session.run() after a load that loadfw reports as done. `still_answers` overrides
        STILL_ANSWERS_SECONDS, so a test of a few seconds can tell an answer near the deadline from an early one."""
        if still_answers is not None:
            with mock.patch.object(hs, "STILL_ANSWERS_SECONDS", still_answers):
                return self.run_load(probe, uptime_before, timeout)
        binary = os.path.join(self.tmp.name, "deluge.bin")
        # A binary path skips the build checks; there's no ELF beside it.
        open(binary, "wb").close()
        lines = []

        class Loading(hs.Session):
            def uptime(self):
                return uptime_before

            def load(self):
                return 0

        s = Loading(
            probe,
            args(no_load=False, build=binary, timeout=timeout),
            self.hw,
            lines.append,
        )
        return s.run(), lines

    def test_the_old_build_still_answering_is_a_failed_load_not_a_hang(self):
        code, lines = self.run_load(UptimesProbe([601] * 1000))
        self.assertEqual(code, hs.EXIT_FAILED)
        self.assertIsNone(self.hw.hung_text())
        self.assertFalse(os.path.exists(self.hw.pending))
        self.assertTrue(
            any(
                "still answers (uptime 601 s)" in line
                and "the load didn't take" in line
                for line in lines
            ),
            lines,
        )
        self.assertFalse(os.path.exists(self.hw.lock))

    def test_a_missed_answer_at_the_end_is_still_a_load_that_didnt_take(self):
        # The old build answered 0.2 s before the deadline (the window is 0.4 s), then one query failed and the wait
        # ran out.
        code, lines = self.run_load(
            OldThenSilentProbe([601, None], start_after=1.4),
            timeout=1.6,
            still_answers=0.4,
        )
        self.assertEqual(code, hs.EXIT_FAILED)
        self.assertIsNone(self.hw.hung_text())
        self.assertTrue(any("the load didn't take" in line for line in lines), lines)

    def test_the_old_build_answering_then_silence_is_a_hang(self):
        # It answered while about to restart; the new build hung at boot, and nothing answered for the rest of the
        # wait (the RAM-loaded hangs of 2026-11-06).
        code, _ = self.run_load(
            OldThenSilentProbe([601]), timeout=1.6, still_answers=0.4
        )
        self.assertEqual(code, hs.EXIT_HUNG)
        self.assertIn("no answer from the test port", self.hw.hung_text())

    def test_silence_after_the_old_build_answered_through_a_miss_is_a_hang(self):
        # Silence for the rest of the wait is a hang, whatever came before.
        code, _ = self.run_load(
            OldThenSilentProbe([601, None, 602]), timeout=1.6, still_answers=0.4
        )
        self.assertEqual(code, hs.EXIT_HUNG)
        self.assertIn("no answer from the test port", self.hw.hung_text())

    def test_the_old_build_answering_through_a_miss_near_the_end_didnt_take(self):
        code, _ = self.run_load(
            OldThenSilentProbe([601, None, 602], start_after=1.35),
            timeout=1.6,
            still_answers=0.4,
        )
        self.assertEqual(code, hs.EXIT_FAILED)
        self.assertIsNone(self.hw.hung_text())

    def test_a_short_timeout_shrinks_the_window(self):
        # 1.6 s against STILL_ANSWERS_SECONDS (5 s): every old answer would be near the end.
        code, _ = self.run_load(OldThenSilentProbe([601]), timeout=1.6)
        self.assertEqual(code, hs.EXIT_HUNG)

    def test_an_old_build_answering_at_every_real_poll_of_a_one_second_wait_didnt_take(
        self,
    ):
        # At the real 0.5 s poll, the last answer comes about 0.5 s before the deadline: the window must hold it.
        with mock.patch.object(hs, "READY_POLL_SECONDS", 0.5):
            code, _ = self.run_load(UptimesProbe([601] * 1000), timeout=1)
        self.assertEqual(code, hs.EXIT_FAILED)
        self.assertIsNone(self.hw.hung_text())

    def test_no_answer_at_all_after_the_load_is_a_hang(self):
        code, _ = self.run_load(OldThenSilentProbe([]))
        self.assertEqual(code, hs.EXIT_HUNG)
        self.assertIn("no answer from the test port", self.hw.hung_text())


class WaitReady(FakeClockBase):
    def run_with(self, error):
        probe = AnswersOnceProbe(error)
        lines = []
        s = hs.Session(probe, args(timeout=1), self.hw, out=lines.append)
        return s.run(), probe

    def test_an_answer_then_silence_ends_hung(self):
        code, probe = self.run_with(TimeoutError("no reply to test port command 0x2"))
        self.assertEqual(code, hs.EXIT_HUNG)
        self.assertIn("no answer from the test port", self.hw.hung_text())
        self.assertTrue(os.path.exists(self.hw.pending))
        self.assertFalse(os.path.exists(self.hw.lock))
        self.assertGreater(probe.closed, 0)

    def test_an_rtmidi_error_counts_as_silence(self):
        RtMidiError = type("RtMidiError", (Exception,), {})
        InvalidUseError = type("InvalidUseError", (RtMidiError, RuntimeError), {})
        code, _ = self.run_with(InvalidUseError("port went away"))
        self.assertEqual(code, hs.EXIT_HUNG)

    def test_another_error_still_raises(self):
        with self.assertRaises(ValueError):
            self.run_with(ValueError("bad state JSON"))
        self.assertFalse(os.path.exists(self.hw.lock))


class Within(unittest.TestCase):
    def test_a_timeout_needs_the_keyword(self):
        self.assertEqual(
            deluge_probe._options(["gen_key", "5"], "line 1"),
            (["gen_key", "5"], 5.0, False),
        )
        self.assertEqual(
            deluge_probe._options(["gen_left", "within=15"], "line 1"),
            (["gen_left"], 15.0, False),
        )

    def test_within_must_come_last(self):
        with self.assertRaises(ValueError):
            deluge_probe._options(["within=3", "gen_left"], "line 1")

    def test_a_bare_number_on_a_state_expect_is_a_script_error(self):
        with self.assertRaises(ValueError):
            deluge_probe._run_lines(
                StatePort(), ["expect playing=false 10"], lambda line: None, None
            )


class StatePort:
    events: ClassVar[list] = []

    def wait_idle(self, timeout=None):
        return {"playing": False}


if __name__ == "__main__":
    unittest.main()
