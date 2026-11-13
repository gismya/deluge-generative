"""Files on the Deluge's SD card over USB: a host client for the firmware's JSON-over-sysex file API.

    ./dbt exec "python3 scripts/generative/deluge_files.py ls /SONGS"
    ./dbt exec "python3 scripts/generative/deluge_files.py put pack/SAMPLES /SAMPLES"
    ./dbt exec "python3 scripts/generative/deluge_files.py get /SONGS/SONG001.XML /tmp/SONG001.XML"
    ./dbt exec "python3 scripts/generative/deluge_files.py rm /GENTEST/a.txt"

Commands:
    ls PATH             one line per entry: "d  NAME/" or "f  SIZE  NAME"
    put LOCAL CARDPATH  a file to CARDPATH (into it if CARDPATH ends in "/"), or a directory's contents under CARDPATH,
                        recursively, making directories as needed. A file whose size on the card already matches is
                        skipped unless --force.
    get CARDPATH LOCAL  a file to LOCAL (into it if LOCAL is a directory), or a directory recursively into LOCAL
    rm CARDPATH         a file; with -r, a directory and everything under it (never "/")

It writes and deletes only under the paths it's given. Every command takes the same lock as `deluge_probe.py session`
(hardware_session.py), so it never runs during a hardware run, and refuses while the hung marker exists (exit 3).

The protocol is src/deluge/storage/smsysex.cpp, reached through SysexCommands::Json: a request is
F0 00 21 7B 01 04 <msgid> {json} [00 <7-bit packed data>] F7, and the reply is F0 00 21 7B 01 05 <msgid> {json}
[00 <packed data>] F7, its JSON one object named after the request with a "^" in front ("^open"), its "err" a FatFs
FRESULT. Only the session reply comes as 04 with msgid 0. The firmware takes at most 1024 bytes per message
(incomingSysexBuffer), so writes go in pieces (USB_BLOCKS, or DIN_BLOCKS with --udp). Any build has the API:
it needs neither the test port nor a RAM-loaded build.
"""

import argparse
import contextlib
import json
import os
import sys
import time
from collections import namedtuple

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from deluge_probe import pack_8to7

HDR = [0xF0, 0x00, 0x21, 0x7B, 0x01]
JSON_REQUEST, JSON_REPLY = 0x04, 0x05  # SysEx::SysexCommands::Json, JsonReply
AM_DIR = 0x10  # FatFs attribute bit of a directory
# What one request moves: bytes read, bytes written, and directory entries listed. Over USB a read returns at most blockBufferMax (1024, smsysex.cpp). A write
# message (its data packed, 8 bytes for 7, after the header and JSON) should fit the 1024 bytes the firmware takes
# (incomingSysexBuffer), but on the desk Deluge (2026-11-07) one of 749 bytes wrote every byte and one of 757 wrote all
# but its last, with the reply's size saying so; the cause isn't known. 512 bytes make a message of at most ~660, also
# with 10-digit numbers in the JSON. Over DIN (delugemu's UDP bridge) the receive ring is 512 bytes
# (MIDI_RX_BUFFER_SIZE) and the send buffer 1024 (MIDI_TX_BUFFER_SIZE), and both drop what doesn't fit. A listing
# asks for MAX_DIR_LINES (25, smsysex.cpp) entries over USB; over DIN 5, so even ~150-character names fit the 1024 bytes.
Blocks = namedtuple("Blocks", "read write dir_lines")
USB_BLOCKS = Blocks(1024, 512, 25)
DIN_BLOCKS = Blocks(512, 320, 5)

# FRESULT in src/fatfs/ff.h, by value.
FRESULT = [
    "FR_OK",
    "FR_DISK_ERR",
    "FR_INT_ERR",
    "FR_NOT_READY",
    "FR_NO_FILE",
    "FR_NO_PATH",
    "FR_INVALID_NAME",
    "FR_DENIED",
    "FR_EXIST",
    "FR_INVALID_OBJECT",
    "FR_WRITE_PROTECTED",
    "FR_INVALID_DRIVE",
    "FR_NOT_ENABLED",
    "FR_NO_FILESYSTEM",
    "FR_MKFS_ABORTED",
    "FR_TIMEOUT",
    "FR_LOCKED",
    "FR_NOT_ENOUGH_CORE",
    "FR_TOO_MANY_OPEN_FILES",
    "FR_INVALID_PARAMETER",
]

Entry = namedtuple("Entry", "name size is_dir")


class CardError(RuntimeError):
    """A request the firmware answered with an error: `what` is the request, `code` its FRESULT, and `detail` what
    else happened (a partial file deleted, say)."""

    def __init__(self, what, path, code, detail=""):
        name = FRESULT[code] if 0 <= code < len(FRESULT) else str(code)
        super().__init__(f"{what} {path}: {name}" + (f"; {detail}" if detail else ""))
        self.what, self.code = what, code


def card_path(path):
    """`path` as the firmware takes it: absolute, "/" between parts, no trailing "/" (except "/" itself)."""
    if not path.isascii():
        # json.dumps writes "é" as \u00e9, and the firmware's reader takes that apart into other characters, "/" among
        # them, so it would land somewhere else on the card.
        raise ValueError(f"{path}: card paths must be ASCII")
    parts = [p for p in path.replace("\\", "/").split("/") if p not in ("", ".")]
    if ".." in parts:
        raise ValueError(f"{path}: no '..' in card paths")
    return "/" + "/".join(parts)


def join(parent, name):
    return card_path(parent + "/" + name)


def split(path):
    """A card path's (parent directory, name): split("/SONGS/A.XML") is ("/SONGS", "A.XML"), split("/A") ("/", "A")."""
    parent, name = card_path(path).rsplit("/", 1)
    return parent or "/", name


class Card:
    """The file API over a link (deluge_probe.RtMidiLink or UdpLink: whole sysex messages in and out)."""

    def __init__(self, link, timeout=5.0, blocks=USB_BLOCKS):
        self.link = link
        self.timeout = timeout
        self.blocks = blocks
        self.msgids = None

    def _session(self):
        """Asks for a session, so this client's message ids don't collide with another's (a browser app's)."""
        self.link.send(
            HDR
            + [JSON_REQUEST, 0]
            + list(b'{"session": {"tag": "deluge_files"}}')
            + [0xF7]
        )
        reply = self._receive(lambda m: m[5] == JSON_REQUEST and m[6] == 0, "session")
        session = json.loads(self._split(reply)[0])["^session"]
        self.msgids = list(range(session["midMin"], session["midMax"] + 1))
        self.next = 0

    def _receive(self, match, what):
        end = time.monotonic() + self.timeout
        while True:
            left = end - time.monotonic()
            if left <= 0:  # also while other messages keep arriving
                raise TimeoutError(f"no reply to {what} within {self.timeout:g} s")
            msg = self.link.receive(left)
            if msg is not None and msg[:5] == HDR and len(msg) > 7 and match(msg):
                return msg

    @staticmethod
    def _split(msg):
        """A reply's (JSON text, packed data or None)."""
        body = bytes(msg[7:-1])
        text, sep, data = body.partition(b"\x00")
        return text.decode("latin-1"), (list(data) if sep else None)

    def request(self, command, args, data=None):
        """Sends one request and returns (the reply's JSON object, its decoded data or None)."""
        if self.msgids is None:
            self._session()
        msgid = self.msgids[self.next % len(self.msgids)]
        self.next += 1
        msg = (
            HDR
            + [JSON_REQUEST, msgid]
            + list(json.dumps({command: args}).encode("latin-1"))
        )
        if data is not None:
            msg += [0] + pack_8to7(data)
        self.link.send(msg + [0xF7])
        reply = self._receive(lambda m: m[5] == JSON_REPLY and m[6] == msgid, command)
        text, packed = self._split(reply)
        return json.loads(text)["^" + command], (
            None if packed is None else unpack_7to8(packed)
        )

    def _open(self, path, write):
        reply, _ = self.request("open", {"path": path, "write": 1 if write else 0})
        if reply["err"]:
            raise CardError("open", path, reply["err"])
        return reply["fid"], reply["size"]

    def _close(self, fid, path):
        reply, _ = self.request("close", {"fid": fid})
        if reply["err"]:
            raise CardError("close", path, reply["err"])

    @contextlib.contextmanager
    def _opened(self, path, write):
        """The file `path` open as (fid, size) for the block; closed after it. After a TimeoutError it sends nothing
        more (the device stopped answering), and a close that fails while another error propagates is dropped: the
        first error is the one to report."""
        fid, size = self._open(path, write)
        try:
            yield fid, size
        except TimeoutError:
            raise
        except BaseException:
            try:
                self._close(fid, path)
            except (CardError, TimeoutError):
                pass
            raise
        self._close(fid, path)

    def read_file(self, path):
        path = card_path(path)
        data = bytearray()
        with self._opened(path, write=False) as (fid, size):
            while len(data) < size:
                reply, block = self.request(
                    "read", {"fid": fid, "addr": len(data), "size": self.blocks.read}
                )
                if reply["err"]:
                    raise CardError("read", path, reply["err"])
                if not block:  # the file ended early
                    raise CardError("read", path, FRESULT.index("FR_DISK_ERR"))
                data += block
        return bytes(data)

    def write_file(self, path, data):
        """Writes `data` to the file `path`, replacing it. The firmware makes missing directories on the way."""
        path = card_path(path)
        try:
            with self._opened(path, write=True) as (fid, _):
                self._write_blocks(fid, path, data)
        except TimeoutError as exc:
            raise TimeoutError(f"{exc}; {path} may be left partly written") from exc
        except CardError as exc:
            if exc.what == "open":
                raise  # nothing written
            raise CardError(
                exc.what, path, exc.code, self._delete_partial(path)
            ) from exc

    def _delete_partial(self, path):
        """Deletes what a failed write left at `path`. Returns what happened, for the error."""
        try:
            reply, _ = self.request("delete", {"path": path})
        except TimeoutError:
            return f"{path} may be left partly written"
        if reply["err"]:
            return f"couldn't delete the partial {path} ({FRESULT[reply['err']]})"
        return f"deleted the partial {path}"

    def _write_blocks(self, fid, path, data):
        for addr in range(0, len(data), self.blocks.write):
            block = data[addr : addr + self.blocks.write]
            reply, _ = self.request(
                "write", {"fid": fid, "addr": addr, "size": len(block)}, block
            )
            if reply["err"]:
                raise CardError("write", path, reply["err"])
            if reply["size"] != len(
                block
            ):  # a short write: the card is full, or see USB_BLOCKS
                raise CardError("write", path, FRESULT.index("FR_DENIED"))

    def ls(self, path):
        """The entries of the directory `path`, in the card's order."""
        path = card_path(path)
        entries = []
        while True:
            reply, _ = self.request(
                "dir",
                {"path": path, "offset": len(entries), "lines": self.blocks.dir_lines},
            )
            if reply["err"]:
                raise CardError("ls", path, reply["err"])
            got = reply["list"]
            entries += [
                Entry(e["name"], e["size"], bool(e["attr"] & AM_DIR)) for e in got
            ]
            if len(got) < self.blocks.dir_lines:
                return entries


def unpack_7to8(data):
    """unpack_7bit_to_8bit() in src/deluge/util/pack.c."""
    out = bytearray()
    for i in range(0, len(data), 8):
        high = data[i]
        out += bytes(
            b | (0x80 if high & (1 << j) else 0)
            for j, b in enumerate(data[i + 1 : i + 8])
        )
    return bytes(out)


# ---- Commands ----


class Usage(Exception):
    """A command that can't go ahead: printed, exit 1."""


def find(card, path):
    """The Entry for `path` ("/" is a directory), or None if it doesn't exist."""
    path = card_path(path)
    if path == "/":
        return Entry("/", 0, True)
    parent, name = split(path)
    try:
        entries = card.ls(parent)
    except CardError as exc:
        if exc.code in (FRESULT.index("FR_NO_PATH"), FRESULT.index("FR_NO_FILE")):
            return None
        raise
    return next(
        (e for e in entries if e.name.lower() == name.lower()), None
    )  # FAT names ignore case


class Tree:
    """What put has seen of the card: each directory's entries, read once, kept as it writes."""

    def __init__(self, card, out):
        self.card, self.out = card, out
        self.dirs = {}

    def entries(self, path):
        """{lower-case name: Entry} for the directory `path`, or None if it doesn't exist."""
        if path not in self.dirs:
            try:
                self.dirs[path] = {e.name.lower(): e for e in self.card.ls(path)}
            except CardError as exc:
                if exc.code != FRESULT.index("FR_NO_PATH"):
                    raise
                self.dirs[path] = None
        return self.dirs[path]

    def ensure_dir(self, path):
        if path == "/":
            return
        parent, name = split(path)
        self.ensure_dir(parent)
        entry = self.entries(parent).get(name.lower())
        if entry is not None:
            if not entry.is_dir:
                raise Usage(f"{path} is a file on the card, not a directory")
            return
        reply, _ = self.card.request("mkdir", {"path": path})
        if reply["err"]:
            raise CardError("mkdir", path, reply["err"])
        self.out(f"mkdir {path}")
        self.entries(parent)[name.lower()] = Entry(name, 0, True)
        self.dirs[path] = {}

    def put_file(self, local, path, force):
        """Writes `local` to `path`. Returns whether it wrote (False: skipped, same size)."""
        parent, name = split(path)
        self.ensure_dir(parent)
        size = os.path.getsize(local)
        existing = self.entries(parent).get(name.lower())
        if existing is not None and existing.is_dir:
            raise Usage(f"{path} is a directory on the card")
        if existing is not None and existing.size == size and not force:
            self.out(f"skip {path} (same size, {size} bytes)")
            return False
        with open(local, "rb") as fh:
            self.card.write_file(path, fh.read())
        self.out(f"put {path} ({size} bytes)")
        self.entries(parent)[name.lower()] = Entry(name, size, False)
        return True


def put(card, local, dest, force, out):
    tree = Tree(card, out)
    jobs = []  # (local file, card path)
    if os.path.isdir(local):
        root = card_path(dest)
        for top, dirs, files in os.walk(local):
            dirs.sort()
            rel = os.path.relpath(top, local)
            for name in sorted(files):
                jobs.append(
                    (os.path.join(top, name), card_path(f"{root}/{rel}/{name}"))
                )
    elif os.path.isfile(local):
        target = card_path(dest)
        if dest.endswith("/") or (find(card, target) or Entry("", 0, False)).is_dir:
            target = join(target, os.path.basename(local))
        jobs.append((local, target))
    else:
        raise Usage(f"no file or directory {local}")
    written = sum(tree.put_file(src, path, force) for src, path in jobs)
    out(f"put: {written} written, {len(jobs) - written} skipped")


def get(card, path, local, out):
    path = card_path(path)
    entry = find(card, path)
    if entry is None:
        raise Usage(f"no such file or directory on the card: {path}")
    if not entry.is_dir:
        target = os.path.join(local, entry.name) if os.path.isdir(local) else local
        data = card.read_file(path)
        with open(target, "wb") as fh:
            fh.write(data)
        out(f"get {path} -> {target} ({len(data)} bytes)")
        return
    os.makedirs(local, exist_ok=True)
    for e in card.ls(path):
        get(card, join(path, e.name), os.path.join(local, e.name), out)


def rm(card, path, recursive, out):
    path = card_path(path)
    if path == "/":
        raise Usage("rm won't delete the card's root")
    entry = find(card, path)
    if entry is None:
        raise Usage(f"no such file or directory on the card: {path}")
    if entry.is_dir and not recursive:
        raise Usage(f"{path} is a directory: rm -r deletes it and everything under it")
    _delete(card, path, entry, out)


def _delete(card, path, entry, out):
    """Deletes `path`, a directory's contents first (FatFs deletes only empty directories)."""
    if entry.is_dir:
        for e in card.ls(path):
            _delete(card, join(path, e.name), e, out)
    reply, _ = card.request("delete", {"path": path})
    if reply["err"]:
        raise CardError("rm", path, reply["err"])
    out(f"rm {path}")


def ls(card, path, out):
    for e in card.ls(path):
        out(f"d  {e.name}/" if e.is_dir else f"f  {e.size:>10}  {e.name}")


def open_rtmidi():
    import deluge_probe

    return deluge_probe.RtMidiLink(quiet=True)


def refuse_if_hung(hw, out):
    """Whether the hung marker exists (then says so): nothing may touch the device until the user power cycles."""
    hung = hw.hung_text()
    if hung is None:
        return False
    out(f"deluge_files: the Deluge is marked hung ({hw.hung}); don't touch it:")
    out("\n".join("  " + line for line in hung.splitlines()))
    return True


def main(argv=None, open_link=None, hw=None, out=print):
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    sub = ap.add_subparsers(dest="command", required=True)
    sub.add_parser("ls").add_argument("path")
    p = sub.add_parser("put")
    p.add_argument("local")
    p.add_argument("cardpath")
    p.add_argument(
        "--force",
        action="store_true",
        help="write files whose size on the card matches too",
    )
    p = sub.add_parser("get")
    p.add_argument("cardpath")
    p.add_argument("local")
    p = sub.add_parser("rm")
    p.add_argument(
        "-r", action="store_true", help="a directory and everything under it"
    )
    p.add_argument("cardpath")
    ap.add_argument(
        "--wait",
        type=float,
        default=1800,
        help="how long to wait for the hardware lock, in seconds",
    )
    ap.add_argument(
        "--no-wait", action="store_true", help="fail at once if the lock is held"
    )
    ap.add_argument(
        "--timeout", type=float, default=10, help="seconds to wait for each reply"
    )
    ap.add_argument(
        "--udp",
        metavar="LISTEN:SEND",
        help="delugemu's MIDI bridge instead of CoreMIDI",
    )
    a = ap.parse_args(argv)

    import hardware_session

    hw = hw or hardware_session.Hardware()
    if refuse_if_hung(hw, out):
        return hardware_session.EXIT_REFUSED
    owner = "deluge_files.py " + " ".join(argv if argv is not None else sys.argv[1:])
    if not hw.acquire(owner, 0 if a.no_wait else a.wait, out):
        return hardware_session.EXIT_FAILED
    link = None
    try:
        # A session may have hung the device, written the marker and released the lock while this waited.
        if refuse_if_hung(hw, out):
            return hardware_session.EXIT_REFUSED
        if open_link is None:
            import deluge_probe

            open_link = (lambda: deluge_probe.UdpLink(a.udp)) if a.udp else open_rtmidi
        link = open_link()
        card = Card(link, a.timeout, DIN_BLOCKS if a.udp else USB_BLOCKS)
        if a.command == "ls":
            ls(card, a.path, out)
        elif a.command == "put":
            put(card, a.local, a.cardpath, a.force, out)
        elif a.command == "get":
            get(card, a.cardpath, a.local, out)
        elif a.command == "rm":
            rm(card, a.cardpath, a.r, out)
        return 0
    except (Usage, CardError, TimeoutError, LookupError, ValueError, OSError) as exc:
        out(f"deluge_files: {exc}")
        return hardware_session.EXIT_FAILED
    finally:
        try:
            if link is not None:
                link.close()
        finally:
            hw.release()


if __name__ == "__main__":
    sys.stdout.reconfigure(
        line_buffering=True
    )  # a long put shows each file as it goes, also into a file
    sys.exit(main())
