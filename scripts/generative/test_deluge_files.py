"""Tests for deluge_files.py without a Deluge: a fake that speaks the firmware's JSON-over-sysex file API.

python3 scripts/generative/test_deluge_files.py
"""

import io
import json
import os
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import deluge_files as df
import hardware_session as hs

HDR = [0xF0, 0x00, 0x21, 0x7B, 0x01]
JSON_REQUEST, JSON_REPLY = 0x04, 0x05
INCOMING_MAX = 1024  # the firmware's incomingSysexBuffer: a longer message is dropped
MAX_DIR_LINES = 25  # smsysex.cpp
READ_MAX = 1024  # smsysex.cpp blockBufferMax
FR_OK, FR_NO_FILE, FR_NO_PATH, FR_DENIED, FR_EXIST, FR_NOT_ENABLED = 0, 4, 5, 7, 8, 12
AM_DIR = 0x10


def pack(data):
    """pack_8bit_to_7bit() in src/deluge/util/pack.c, written out again for the fake."""
    out = []
    for i in range(0, len(data), 7):
        chunk = data[i : i + 7]
        out.append(sum(1 << j for j, b in enumerate(chunk) if b & 0x80))
        out += [b & 0x7F for b in chunk]
    return out


def unpack(data):
    """unpack_7bit_to_8bit() in src/deluge/util/pack.c."""
    out = bytearray()
    for i in range(0, len(data), 8):
        high = data[i]
        for j, b in enumerate(data[i + 1 : i + 8]):
            out.append(b | (0x80 if high & (1 << j) else 0))
    return bytes(out)


def attrs(**values):
    """Attributes as JsonSerializer::writeAttribute() writes them: ` "name": value`, comma first after the first."""
    return ",".join(f' "{k}": {json.dumps(v)}' for k, v in values.items())


class FakeCard:
    """The card behind smsysex.cpp, in memory: files are bytes, directories are dicts. Replies are byte-for-byte what
    the firmware's JsonSerializer writes. A message longer than the firmware's incoming buffer is dropped, as there."""

    def __init__(self, incoming_max=INCOMING_MAX, outgoing_max=None):
        self.incoming_max, self.outgoing_max = incoming_max, outgoing_max
        self.root = {}
        self.files = {}  # fid -> [path, write]
        self.next_fid = 1
        self.sent = []  # every request, as sent
        self.inbox = []
        self.sessions = 0
        self.drop_replies = False
        self.full_after = (
            None  # the card fills up once a file reaches this many bytes: short writes
        )
        self.close_err = None  # every close answers this FRESULT
        self.silent_after_writes = None  # no replies at all after this many writes (the device stopped answering)
        self.writes = 0

    # -- the card --

    def _walk(self, path):
        """(parent dict, name) for `path`, or (None, name) if a parent is missing."""
        parts = [p for p in path.split("/") if p]
        node = self.root
        for part in parts[:-1]:
            node = node.get(part)
            if not isinstance(node, dict):
                return None, parts[-1] if parts else ""
        return node, parts[-1] if parts else ""

    def get(self, path):
        if path.strip("/") == "":
            return self.root
        parent, name = self._walk(path)
        return None if parent is None else parent.get(name)

    def put(self, path, value):
        """Test setup: puts a file (bytes) or directory ({}) at `path`, making its parents."""
        node = self.root
        parts = [p for p in path.split("/") if p]
        for part in parts[:-1]:
            node = node.setdefault(part, {})
        node[parts[-1]] = value

    # -- the link --

    def send(self, msg):
        self.sent.append(list(msg))
        if len(msg) > self.incoming_max:
            return
        assert msg[:5] == HDR and msg[5] == JSON_REQUEST and msg[-1] == 0xF7, msg[:8]
        seq = msg[6]
        body = bytes(msg[7:-1])
        text, sep, data = body.partition(b"\x00")
        request = json.loads(text)
        ((command, args),) = request.items()
        reply, payload = getattr(self, "do_" + command)(
            args, unpack(list(data)) if sep else b""
        )
        if command == "write":
            self.writes += 1
            if (
                self.silent_after_writes is not None
                and self.writes > self.silent_after_writes
            ):
                self.drop_replies = True
        if reply is None or self.drop_replies:
            return
        out = HDR + [JSON_REPLY if command != "session" else JSON_REQUEST]
        out += [seq if command != "session" else 0]
        out += list(reply.encode("latin-1"))
        if payload is not None:
            out += [0] + pack(payload)
        if self.outgoing_max is not None and len(out) + 1 > self.outgoing_max:
            return  # the firmware's send buffer drops what doesn't fit
        self.inbox.append(out + [0xF7])

    def receive(self, timeout):
        return self.inbox.pop(0) if self.inbox else None

    def close(self):
        pass

    # -- the commands, as smsysex.cpp answers them --

    def do_session(self, args, data):
        self.sessions += 1
        sid = 1
        return (
            '{"^session": {'
            + attrs(sid=sid, tag=args.get("tag", ""), midBase=8, midMin=9, midMax=15)
            + "}}",
            None,
        )

    def do_dir(self, args, data):
        node = self.get(args.get("path", "/"))
        offset, lines = args.get("offset", 0), min(args.get("lines", 20), MAX_DIR_LINES)
        entries = []
        err = FR_OK
        if not isinstance(node, dict):
            err = FR_NO_PATH
        else:
            for name, value in list(node.items())[offset : offset + lines]:
                is_dir = isinstance(value, dict)
                entries.append(
                    "{"
                    + attrs(
                        name=name,
                        size=0 if is_dir else len(value),
                        date=0,
                        time=0,
                        attr=AM_DIR if is_dir else 0x20,
                    )
                    + "}"
                )
        text = (
            '{"^dir": {\n"list": [' + ",".join(entries) + "\n]," + attrs(err=err) + "}}"
        )
        return text, None

    def do_open(self, args, data):
        path, write = args["path"], args.get("write", 0)
        parent, name = self._walk(path)
        if write and parent is None:
            # openFile() makes the missing directories, then tries again.
            node = self.root
            for part in [p for p in path.split("/") if p][:-1]:
                node = node.setdefault(part, {})
            parent, name = self._walk(path)
        if parent is None:
            err, fid, size = FR_NO_PATH, 0, 0
        elif write:
            parent[name] = b""
            err, fid, size = FR_OK, self.next_fid, 0
        elif not isinstance(parent.get(name), bytes):
            err, fid, size = FR_NO_FILE, 0, 0
        else:
            err, fid, size = FR_OK, self.next_fid, len(parent[name])
        if fid:
            self.files[fid] = [path, write]
            self.next_fid += 1
        return '{"^open": {' + attrs(fid=fid, size=size, err=err) + "}}", None

    def do_close(self, args, data):
        err = FR_OK if self.files.pop(args["fid"], None) else 9
        if self.close_err is not None:
            err = self.close_err
        return '{"^close": {' + attrs(fid=args["fid"], err=err) + "}}", None

    def do_read(self, args, data):
        fid, addr, size = (
            args["fid"],
            args["addr"],
            min(args.get("size", READ_MAX), READ_MAX),
        )
        if fid not in self.files:
            return '{"^read": {' + attrs(
                fid=fid, addr=addr, size=0, err=FR_NOT_ENABLED
            ) + "}}", b""
        chunk = self.get(self.files[fid][0])[addr : addr + size]
        return '{"^read": {' + attrs(
            fid=fid, addr=addr, size=len(chunk), err=FR_OK
        ) + "}}", chunk

    def do_write(self, args, data):
        fid, addr = args["fid"], args["addr"]
        if fid not in self.files:
            return '{"^write": {' + attrs(
                fid=fid, addr=addr, size=args["size"], err=FR_NOT_ENABLED
            ) + "}}", None
        path = self.files[fid][0]
        parent, name = self._walk(path)
        old = parent[name]
        assert addr == len(old), "the fake only appends"
        data = data[: args["size"]]
        if self.full_after is not None:
            data = data[: max(0, self.full_after - len(old))]
        parent[name] = old + data
        return '{"^write": {' + attrs(
            fid=fid, addr=addr, size=len(data), err=FR_OK
        ) + "}}", None

    def do_delete(self, args, data):
        parent, name = self._walk(args["path"])
        if parent is None or name not in parent:
            err = FR_NO_FILE
        elif isinstance(parent[name], dict) and parent[name]:
            err = FR_DENIED  # f_unlink on a directory that isn't empty
        else:
            del parent[name]
            err = FR_OK
        return '{"^delete": {' + attrs(err=err) + "}}", None

    def do_mkdir(self, args, data):
        parent, name = self._walk(args["path"])
        if parent is None:
            err = FR_NO_PATH
        elif name in parent:
            err = FR_EXIST
        else:
            parent[name] = {}
            err = FR_OK
        return '{"^mkdir": {' + attrs(path=args["path"], err=err) + "}}", None

    def requests(self, command):
        """The JSON of every request for `command` that the fake took."""
        out = []
        for msg in self.sent:
            if len(msg) > self.incoming_max:
                continue
            text = bytes(msg[7:-1]).partition(b"\x00")[0]
            request = json.loads(text)
            if command in request:
                out.append(request[command])
        return out


class Base(unittest.TestCase):
    def setUp(self):
        self.fake = FakeCard()
        self.card = df.Card(self.fake, timeout=0.01)
        self.tmp = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.tmp.cleanup()

    def local(self, rel, data=None):
        path = os.path.join(self.tmp.name, rel)
        if data is not None:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as fh:
                fh.write(data)
        return path


class List(Base):
    def test_lists_a_directory_past_one_reply(self):
        for i in range(30):
            self.fake.put(f"/SONGS/SONG{i:03}.XML", b"x" * i)
        self.fake.put("/SONGS/OLD", {})
        entries = self.card.ls("/SONGS")
        self.assertEqual(len(entries), 31)
        self.assertEqual(entries[29], df.Entry("SONG029.XML", 29, False))
        self.assertEqual(entries[30], df.Entry("OLD", 0, True))

    def test_a_missing_directory_is_an_error(self):
        with self.assertRaises(df.CardError) as caught:
            self.card.ls("/NOPE")
        self.assertIn("FR_NO_PATH", str(caught.exception))


class Transfer(Base):
    def test_a_file_comes_back_byte_identical(self):
        data = (
            bytes(range(256)) * 13 + b"\xff\x80\x00"
        )  # every byte value, over several blocks, an odd tail
        self.card.write_file("/GENTEST/blob.bin", data)
        self.assertEqual(self.fake.get("/GENTEST/blob.bin"), data)
        self.assertEqual(self.card.read_file("/GENTEST/blob.bin"), data)
        self.assertEqual(self.fake.files, {}, "every file it opened is closed")

    def test_every_write_stays_under_what_the_device_takes_whole(self):
        # On the desk Deluge (2026-11-07) a 749-byte write message wrote every byte, a 757-byte one all but its last.
        self.fake.next_fid = 2**31 - 10  # the longest numbers the JSON can carry
        self.card.write_file("/GENTEST/big.bin", b"\xff" * 5000)
        self.assertLessEqual(max(len(m) for m in self.fake.sent), 749)
        self.assertEqual(self.fake.get("/GENTEST/big.bin"), b"\xff" * 5000)

    def test_over_din_every_message_fits_its_smaller_buffers(self):
        # delugemu's bridge is the DIN port: a 512-byte receive ring (MIDI_RX_BUFFER_SIZE), a 1024-byte send buffer.
        fake = FakeCard(incoming_max=512)
        card = df.Card(fake, timeout=0.01, blocks=df.DIN_BLOCKS)
        fake.next_fid = 2**31 - 10
        data = bytes(range(256)) * 8
        card.write_file("/GENTEST/din.bin", data)
        self.assertEqual(card.read_file("/GENTEST/din.bin"), data)
        self.assertLessEqual(max(len(m) for m in fake.sent), 512)
        self.assertTrue(all(r["size"] <= 512 for r in fake.requests("read")))

    def test_over_din_a_listing_of_long_names_fits_the_send_buffer(self):
        # MIDI_TX_BUFFER_SIZE is 1024 bytes: a reply with 25 entries of 60-character names wouldn't fit.
        fake = FakeCard(incoming_max=512, outgoing_max=1024)
        for i in range(30):
            fake.put(f"/SONGS/{'A LONG SONG NAME ' * 3}{i:03}.XML", b"x")
        card = df.Card(fake, timeout=0.01, blocks=df.DIN_BLOCKS)
        self.assertEqual(len(card.ls("/SONGS")), 30)

    def test_an_empty_file(self):
        self.card.write_file("/GENTEST/empty", b"")
        self.assertEqual(self.card.read_file("/GENTEST/empty"), b"")

    def test_reading_a_missing_file_is_an_error(self):
        self.fake.put("/GENTEST", {})
        with self.assertRaises(df.CardError) as caught:
            self.card.read_file("/GENTEST/none")
        self.assertIn("FR_NO_FILE", str(caught.exception))


class Commands(Base):
    """deluge_files.py's commands, through main(), with the fake as the link and the lock files in a temp dir."""

    def setUp(self):
        super().setUp()
        self.hw = hs.Hardware(os.path.join(self.tmp.name, "hw"))

    def run_main(self, *argv):
        out = io.StringIO()
        with redirect_stdout(out):
            code = df.main(
                ["--timeout", "0.01", "--no-wait", *argv],
                open_link=lambda: self.fake,
                hw=self.hw,
            )
        return code, out.getvalue()

    def test_put_a_directory_makes_its_directories_and_skips_files_of_the_same_size(
        self,
    ):
        self.local("pack/SAMPLES/FX/Riser/a.wav", b"A" * 1000)
        self.local("pack/SYNTHS/GENERATIVE/FX/GEN X.XML", b"<sound/>")
        self.fake.put("/SAMPLES/DRUMS/kick.wav", b"keep")
        code, out = self.run_main("put", self.local("pack"), "/")
        self.assertEqual(code, 0, out)
        self.assertEqual(self.fake.get("/SAMPLES/FX/Riser/a.wav"), b"A" * 1000)
        self.assertEqual(self.fake.get("/SYNTHS/GENERATIVE/FX/GEN X.XML"), b"<sound/>")
        self.assertEqual(self.fake.get("/SAMPLES/DRUMS/kick.wav"), b"keep")
        self.assertIn("put: 2 written, 0 skipped", out)

        self.fake.sent.clear()
        code, out = self.run_main("put", self.local("pack"), "/")
        self.assertEqual(code, 0, out)
        self.assertIn("put: 0 written, 2 skipped", out)
        self.assertEqual(
            self.fake.requests("open"), [], "nothing opened when every size matches"
        )

    def test_put_force_writes_a_file_of_the_same_size(self):
        self.local("pack/a.txt", b"new")
        self.fake.put("/GENTEST/a.txt", b"old")
        code, out = self.run_main("put", "--force", self.local("pack"), "/GENTEST")
        self.assertEqual(code, 0, out)
        self.assertEqual(self.fake.get("/GENTEST/a.txt"), b"new")

    def test_put_a_file_into_a_directory_or_to_a_name(self):
        src = self.local("x/note.txt", b"hello")
        self.assertEqual(self.run_main("put", src, "/GENTEST/")[0], 0)
        self.assertEqual(self.fake.get("/GENTEST/note.txt"), b"hello")
        self.assertEqual(self.run_main("put", src, "/GENTEST/other.txt")[0], 0)
        self.assertEqual(self.fake.get("/GENTEST/other.txt"), b"hello")

    def test_put_writes_only_under_the_path_given(self):
        self.local("pack/SAMPLES/a.wav", b"a")
        self.run_main("put", self.local("pack"), "/GENTEST")
        written = {r["path"] for r in self.fake.requests("open") if r["write"]}
        made = {r["path"] for r in self.fake.requests("mkdir")}
        self.assertEqual(written, {"/GENTEST/SAMPLES/a.wav"})
        self.assertTrue(all(p.startswith("/GENTEST") for p in made), made)

    def test_get_a_file_and_a_directory(self):
        self.fake.put("/SONGS/A.XML", b"<song a/>")
        self.fake.put("/SONGS/SUB/B.XML", b"<song b/>")
        dest = self.local("got")
        os.makedirs(dest)
        self.assertEqual(self.run_main("get", "/SONGS/A.XML", dest)[0], 0)
        with open(os.path.join(dest, "A.XML"), "rb") as fh:
            self.assertEqual(fh.read(), b"<song a/>")
        self.assertEqual(self.run_main("get", "/SONGS", self.local("tree"))[0], 0)
        with open(self.local("tree/SUB/B.XML"), "rb") as fh:
            self.assertEqual(fh.read(), b"<song b/>")

    def test_rm_deletes_a_file_but_a_directory_only_with_r(self):
        self.fake.put("/GENTEST/a.txt", b"a")
        self.fake.put("/GENTEST/sub/b.txt", b"b")
        self.fake.put("/KEEP/c.txt", b"c")
        code, out = self.run_main("rm", "/GENTEST")
        self.assertEqual(code, 1)
        self.assertIn("is a directory", out)
        self.assertEqual(self.run_main("rm", "/GENTEST/a.txt")[0], 0)
        self.assertIsNone(self.fake.get("/GENTEST/a.txt"))
        self.assertEqual(self.run_main("rm", "-r", "/GENTEST")[0], 0)
        self.assertIsNone(self.fake.get("/GENTEST"))
        self.assertEqual(self.fake.get("/KEEP/c.txt"), b"c")
        deleted = [r["path"] for r in self.fake.requests("delete")]
        self.assertTrue(all(p.startswith("/GENTEST") for p in deleted), deleted)

    def test_rm_r_refuses_the_root(self):
        self.fake.put("/KEEP/c.txt", b"c")
        code, out = self.run_main("rm", "-r", "/")
        self.assertEqual(code, 1)
        self.assertIn("won't delete the card's root", out)
        self.assertEqual(self.fake.requests("delete"), [])

    def test_rm_a_missing_file_is_an_error(self):
        self.fake.put("/GENTEST", {})
        code, out = self.run_main("rm", "/GENTEST/none")
        self.assertEqual(code, 1)
        self.assertIn("no such file", out)

    def test_refuses_while_the_device_is_marked_hung(self):
        self.hw.write_hung("test", None, [])
        code, out = self.run_main("ls", "/")
        self.assertEqual(code, 3)
        self.assertIn("marked hung", out)
        self.assertEqual(self.fake.sent, [])

    def test_refuses_when_the_device_was_marked_hung_while_it_waited(self):
        # A session hangs the device, writes the marker and releases the lock to this waiting command.
        acquire = self.hw.acquire

        def hung_meanwhile(*args, **kw):
            self.hw.write_hung("hung during a session", None, [])
            return acquire(*args, **kw)

        self.hw.acquire = hung_meanwhile
        code, out = self.run_main("ls", "/")
        self.assertEqual(code, 3)
        self.assertIn("marked hung", out)
        self.assertEqual(self.fake.sent, [])
        self.assertFalse(os.path.exists(self.hw.lock), "the lock is released")

    def test_waits_for_a_held_lock_and_releases_its_own(self):
        other = hs.Hardware(self.hw.root)
        self.assertTrue(other.acquire("someone else", 0))
        code, out = self.run_main("ls", "/")
        self.assertEqual(code, 1)
        self.assertIn("in use", out)
        self.assertEqual(self.fake.sent, [])
        other.release()
        code, out = self.run_main("ls", "/")
        self.assertEqual(code, 0, out)
        self.assertFalse(os.path.exists(self.hw.lock), "the lock is released")


class Failures(Commands):
    """What a put or get leaves behind, and what it says, when something fails partway."""

    def test_a_full_card_deletes_the_partial_file_and_names_it(self):
        self.fake.full_after = 1000
        code, out = self.run_main(
            "put", self.local("x/big.bin", b"b" * 3000), "/GENTEST/"
        )
        self.assertEqual(code, 1)
        self.assertIn("write /GENTEST/big.bin: FR_DENIED", out)
        self.assertIn("deleted the partial /GENTEST/big.bin", out)
        self.assertIsNone(self.fake.get("/GENTEST/big.bin"))

    def test_a_failed_close_doesnt_hide_the_write_error(self):
        self.fake.full_after = 1000
        self.fake.close_err = 1  # FR_DISK_ERR
        code, out = self.run_main(
            "put", self.local("x/big.bin", b"b" * 3000), "/GENTEST/"
        )
        self.assertEqual(code, 1)
        self.assertIn("write /GENTEST/big.bin: FR_DENIED", out)
        self.assertNotIn("close", out)

    def test_after_a_timeout_it_sends_nothing_more_and_names_the_partial_file(self):
        self.fake.silent_after_writes = 1
        code, out = self.run_main(
            "put", self.local("x/big.bin", b"b" * 3000), "/GENTEST/"
        )
        self.assertEqual(code, 1)
        self.assertIn("no reply to write", out)
        self.assertIn("/GENTEST/big.bin may be left partly written", out)
        last = self.fake.sent[-1]
        self.assertIn(
            b'"write"',
            bytes(last[7:40]),
            "nothing sent after the write that got no reply",
        )

    def test_a_failed_close_after_a_read_doesnt_hide_the_read_error(self):
        self.fake.put("/GENTEST/a.bin", b"a" * 10)
        self.fake.close_err = 1
        self.fake.silent_after_writes = None
        code, out = self.run_main("get", "/GENTEST/a.bin", self.local("got.bin"))
        self.assertEqual(code, 1)
        self.assertIn(
            "close /GENTEST/a.bin: FR_DISK_ERR", out
        )  # with no other error, the close's own is reported

    def test_put_refuses_a_name_that_isnt_ascii_before_writing_anything(self):
        self.local("pack/a.txt", b"a")
        self.local("pack/caf\u00e9.txt", b"b")
        code, out = self.run_main("put", self.local("pack"), "/GENTEST")
        self.assertEqual(code, 1)
        self.assertIn("ASCII", out)
        self.assertEqual(self.fake.requests("open"), [])
        self.assertEqual(self.fake.requests("mkdir"), [])

    def test_a_card_path_must_be_ascii(self):
        with self.assertRaises(ValueError) as caught:
            df.card_path("/SAMPLES/caf\u00e9.wav")
        self.assertIn("ASCII", str(caught.exception))

    def test_the_lock_is_released_when_closing_the_link_fails(self):
        def failing_close():
            raise OSError("the port went away")

        self.fake.close = failing_close
        with self.assertRaises(OSError):
            self.run_main("ls", "/")
        self.assertFalse(os.path.exists(self.hw.lock))


class Receive(unittest.TestCase):
    def test_a_link_that_never_stops_sending_other_messages_still_times_out(self):
        class Chatty:
            calls = 0

            def send(self, msg):
                pass

            def receive(self, timeout):
                Chatty.calls += 1
                if Chatty.calls > 100000:
                    raise AssertionError("never timed out")
                return HDR + [
                    0x03,
                    0x40,
                    0x01,
                    0x41,
                    0xF7,
                ]  # a debug log line, never a reply

        with self.assertRaises(TimeoutError):
            df.Card(Chatty(), timeout=0.01).ls("/")


if __name__ == "__main__":
    unittest.main()
