"""Tests for hardware_session.py (deluge_probe.py session) without a Deluge: the state files and the run's rules.

python3 scripts/generative/test_hardware_session.py
"""

import argparse
import os
import socket
import sys
import tempfile
import unittest
from typing import ClassVar

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
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
        "key": None,
        "key_file": None,
    }
    base.update(kw)
    return argparse.Namespace(**base)


class FakeLink:
    def close(self):
        pass


class FakePort:
    link = FakeLink()


class FakeSession(hs.Session):
    """The device steps scripted: ready gives a port or None, each script returns or raises."""

    def __init__(self, hw, ready=True, scripts=None, **kw):
        a = args(**kw)
        super().__init__(deluge_probe, a, hw, out=self.lines.append)
        self.ready, self.results, self.loads = ready, scripts or {}, 0

    lines: ClassVar[list]

    def wait_ready(self, fresh):
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
        os.makedirs(self.hw.lock)
        with open(os.path.join(self.hw.lock, "owner"), "w") as fh:
            fh.write(f"pid=999999\nhost={socket.gethostname()}\n")
        self.assertTrue(self.hw.acquire("x", 0, out=lambda m: None))
        self.assertTrue(any("stale-lock" in n for n in self.released()))


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
        scripts = {"a.txt": TimeoutError("no reply to test port command 0x10")}
        s = FakeSession(self.hw, scripts=scripts, args=["a.txt", "b.txt"])
        self.assertEqual(s.run(), hs.EXIT_HUNG)
        self.assertIn("during a.txt", self.hw.hung_text())
        self.assertFalse(any("b.txt:" in line for line in s.lines))

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


class AnswersOnceProbe:
    """deluge_probe as seen by Session.wait_ready: the device answers wait_ready() once, then its state() raises."""

    link_failed = staticmethod(deluge_probe.link_failed)
    read_key = staticmethod(deluge_probe.read_key)

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


class WaitReady(Base):
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
            deluge_probe._within(["gen_key", "5"], "line 1"), (["gen_key", "5"], 5.0)
        )
        self.assertEqual(
            deluge_probe._within(["gen_left", "within=15"], "line 1"),
            (["gen_left"], 15.0),
        )

    def test_within_must_come_last(self):
        with self.assertRaises(ValueError):
            deluge_probe._within(["within=3", "gen_left"], "line 1")

    def test_a_bare_number_on_a_state_expect_is_a_script_error(self):
        with self.assertRaises(ValueError):
            deluge_probe._run_lines(
                StatePort(), ["expect playing=false 10"], lambda line: None, None
            )


class StatePort:
    events: ClassVar[list] = []

    def wait_idle(self):
        return {"playing": False}


def _elf(path, segments):
    """A minimal ELF32 with one PT_LOAD program header per (vaddr, filesz)."""
    import struct

    phoff, phentsize = 52, 32
    head = bytearray(52)
    head[:5] = b"\x7fELF\x01"
    struct.pack_into("<I", head, 28, phoff)
    struct.pack_into("<HH", head, 42, phentsize, len(segments))
    body = b"".join(struct.pack("<8I", 1, 0, v, v, n, n, 0, 0) for v, n in segments)
    with open(path, "wb") as fh:
        fh.write(bytes(head) + body)


class ImageEnd(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.bin = os.path.join(self.tmp.name, "deluge.bin")
        open(self.bin, "wb").close()
        self.elf = os.path.join(self.tmp.name, "deluge.elf")

    def test_image_past_page_2_is_refused(self):
        # The tip build that hung: code from 0x2005fb80 to 0x202005b4, stack (no file bytes) higher still.
        _elf(
            self.elf,
            [
                (0x2005FB80, 0x202005B4 - 0x2005FB80),
                (0x202F8000, 0),
                (0x0C000000, 0x1000),
            ],
        )
        self.assertEqual(hs.image_end(self.elf), 0x202005B4)
        self.assertIn("ends at 0x202005b4", hs.check_image_end(self.bin))

    def test_image_below_page_2_loads(self):
        _elf(self.elf, [(0x2005D820, 0x201F8274 - 0x2005D820)])
        self.assertIsNone(hs.check_image_end(self.bin))

    def test_unreadable_elf_doesnt_block(self):
        self.assertIsNone(hs.check_image_end(self.bin))


if __name__ == "__main__":
    unittest.main()
