"""Tests for emu_slots.py: the host-wide pool of real-time emulator slots (#417 slice 2).

python3 scripts/generative/test_emu_slots.py
"""

import os
import subprocess
import sys
import tempfile
import threading
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import emu_slots


class PoolSize(unittest.TestCase):
    def test_defaults_to_six(self):
        self.assertEqual(emu_slots.pool_size({}), 6)

    def test_the_env_var_overrides_it(self):
        self.assertEqual(emu_slots.pool_size({emu_slots.SIZE_ENV: "2"}), 2)

    def test_a_bad_value_is_refused(self):
        for bad in ("0", "-1", "two", ""):
            with self.assertRaises(ValueError, msg=bad):
                emu_slots.pool_size({emu_slots.SIZE_ENV: bad})


class Take(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.folder = self.tmp.name
        self.said = []

    def tearDown(self):
        self.tmp.cleanup()

    def take(self, size=2, **kw):
        return emu_slots.take(
            size, folder=self.folder, out=self.said.append, poll=0.05, **kw
        )

    def test_a_free_pool_gives_a_slot_at_once(self):
        with self.take() as slot:
            self.assertIn(slot.index, (0, 1))
            self.assertLess(slot.waited, 1)
        self.assertFalse(any("waiting" in s for s in self.said))

    def test_holders_get_different_slots(self):
        with self.take() as a, self.take() as b:
            self.assertNotEqual(a.index, b.index)

    def test_a_full_pool_waits_says_so_and_takes_the_slot_freed(self):
        first = self.take(size=1)
        first.__enter__()
        got = {}

        def second():
            with self.take(size=1) as slot:
                got["slot"] = slot

        t = threading.Thread(target=second)
        t.start()
        time.sleep(0.3)
        self.assertNotIn("slot", got)  # still waiting
        first.__exit__(None, None, None)
        t.join(5)
        self.assertEqual(got["slot"].index, 0)
        self.assertGreaterEqual(got["slot"].waited, 0.25)
        self.assertTrue(
            any("waiting for a real-time emulator slot" in s for s in self.said)
        )
        self.assertTrue(any(emu_slots.WAITED.search(s) for s in self.said))

    def test_another_process_holding_every_slot_counts(self):
        # flock is per open file and process: a holder in another process (another worktree's check_all) blocks too
        holder = subprocess.Popen(
            [
                sys.executable,
                "-c",
                (
                    "import sys, time; sys.path.insert(0, sys.argv[1]); import emu_slots\n"
                    "with emu_slots.take(1, folder=sys.argv[2], out=lambda s: None):\n"
                    "    print('held', flush=True); time.sleep(30)"
                ),
                HERE,
                self.folder,
            ],
            stdout=subprocess.PIPE,
            text=True,
        )
        try:
            self.assertEqual(holder.stdout.readline().strip(), "held")
            self.assertIsNone(emu_slots.try_take(1, self.folder))
            # a crashed holder frees its slot: the kernel drops its flock
            holder.kill()
            holder.wait(5)
            slot = emu_slots.try_take(1, self.folder)
            self.assertIsNotNone(slot)
            slot.release()
        finally:
            holder.kill()
            holder.stdout.close()

    def test_the_fd_can_pass_to_a_child_which_keeps_the_slot(self):
        with self.take(size=1) as slot:
            child = subprocess.Popen(
                ["sleep", "30"], pass_fds=[slot.fd], start_new_session=True
            )
        try:
            # the parent let go, but the emulator it started still holds the slot
            self.assertIsNone(emu_slots.try_take(1, self.folder))
        finally:
            child.kill()
            child.wait(5)
        slot = emu_slots.try_take(1, self.folder)
        self.assertIsNotNone(slot)
        slot.release()

    def test_the_log_records_each_take_and_release(self):
        with self.take(), self.take():
            pass
        log = emu_slots.read_log(self.folder)
        self.assertEqual([e[1] for e in log], ["take", "take", "release", "release"])


class Peak(unittest.TestCase):
    def test_the_most_held_at_once(self):
        lines = [
            "1.0 take 0 11",
            "2.0 take 1 12",
            "3.0 release 0 11",
            "4.0 take 0 13",
            "5.0 take 2 14",
            "6.0 release 1 12",
        ]
        self.assertEqual(emu_slots.peak(emu_slots.parse_log(lines)), 3)

    def test_a_slot_taken_again_without_a_release_counts_once(self):
        # a holder killed with SIGKILL logs no release: its slot's next take replaces it
        lines = ["1.0 take 0 11", "2.0 take 0 12", "3.0 take 1 13"]
        self.assertEqual(emu_slots.peak(emu_slots.parse_log(lines)), 2)

    def test_since_counts_from_then_with_what_was_held_then(self):
        lines = [
            "1.0 take 0 11",
            "2.0 take 1 12",
            "2.5 take 2 13",
            "3.0 release 0 11",
            "3.2 release 1 12",
            "5.0 take 0 14",
        ]
        log = emu_slots.parse_log(lines)
        self.assertEqual(emu_slots.peak(log), 3)
        self.assertEqual(
            emu_slots.peak(log, since=4.0), 2
        )  # slot 2, never released, and slot 0's take
        self.assertEqual(
            emu_slots.peak(log[:-1], since=4.0), 1
        )  # slot 2, held since before the window

    def test_bad_lines_are_skipped(self):
        self.assertEqual(
            emu_slots.parse_log(["junk", "1.0 take 0 11"]), [(1.0, "take", 0, 11)]
        )


if __name__ == "__main__":
    unittest.main()
