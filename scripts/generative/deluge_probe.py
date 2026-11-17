"""Deluge USB-MIDI probe and sysex test port client.

Commands:
    probe            (default) read-only: identity, ping, one OLED dump, optionally the debug log
    state            print the test port's state JSON
    inject LINE...   queue input events, e.g. "button SHIFT down", "pad 3 4 down", "encoder 5 2", "wait 100"
    events           print test events (and with --log, debug log lines) for --seconds
    run SCRIPT       run a test script (see run_script), exit 1 if an expect fails
    ready            wait until the test port answers and the song has loaded, e.g. after loadfw (--timeout)
    session SCRIPT...  a whole hardware run on the shared Deluge: hung marker, lock, loadfw (--build BUILD, or
                     --no-load), ready, then every script on that one load (see hardware_session.py)

state, inject, events, run and ready need a build configured with -DENABLE_SYSEX_TEST=ON (see
src/deluge/io/midi/test_port.h). The probe sends nothing to ports whose name doesn't contain "deluge".

--udp LISTEN:SEND talks to delugemu's MIDI bridge instead of CoreMIDI, as raw MIDI bytes on 127.0.0.1: launch the
emulator with --midi udp:127.0.0.1:LISTEN@127.0.0.1:SEND. Use the DIN port: delugemu's USB-MIDI model never delivers
host input to the firmware.

Run with the dbt toolchain python (has python-rtmidi 1.5.8):
    ./dbt exec "python3 scripts/generative/deluge_probe.py [--log-seconds N] [--selftest]"
    ./dbt exec "python3 scripts/generative/deluge_probe.py run scripts/generative/test_port_smoke.txt"
    ./dbt exec "python3 scripts/generative/deluge_probe.py session --build release SCRIPT..."
See docs/hardware-measurements/deluge-usb-control.md on task/hardware-measurements.
"""

import argparse
import collections
import contextlib
import json
import os
import random
import re
import socket
import sys
import threading
import time

IDENTITY_REQ = [0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7]
HDR = [0xF0, 0x00, 0x21, 0x7B, 0x01]
PING = HDR + [0x00, 0xF7]
OLED_RLE = HDR + [0x02, 0x00, 0x01, 0xF7]
OLED_REPLY_HDR = HDR + [0x02, 0x40, 0x01]
LOG_ON = HDR + [0x03, 0x00, 0x01, 0xF7]
LOG_OFF = HDR + [0x03, 0x00, 0x00, 0xF7]

# Test port (src/deluge/io/midi/test_port.h)
INJECT, STATE, STATUS_REPLY, STATE_REPLY = 0x10, 0x11, 0x50, 0x51
QUEUED, QUEUE_FULL, PARSE_ERROR, BAD_KEY = 0, 1, 2, 3  # a status reply's code
STATUS = {
    QUEUED: "queued",
    QUEUE_FULL: "queue full",
    PARSE_ERROR: "parse error",
    BAD_KEY: "bad key",
}
LOG_HDR = HDR + [0x03, 0x40, 0x00]
# Song position, clock, Start, Continue, Stop: see RtMidiLink.
REALTIME_TO_PORT_1 = (0xF2, 0xF8, 0xFA, 0xFB, 0xFC)
EVENT_HDR = HDR + [0x03, 0x40, 0x01]


def unpack_7to8_rle(src):
    """Port of unpack_7to8_rle() in src/deluge/util/pack.c."""
    dst, s = bytearray(), 0
    while s + 1 < len(src):
        first = src[s]
        s += 1
        if first < 64:
            for lim, size, off in ((4, 2, 0), (12, 3, 4), (28, 4, 12), (60, 5, 28)):
                if first < lim:
                    break
            else:
                raise ValueError("bad dense header")
            hb = first - off
            for j in range(size):
                dst.append((src[s + j] & 0x7F) | (0x80 if hb & (1 << j) else 0))
            s += size
        else:
            first -= 64
            high, run = first & 1, first >> 1
            if run == 31:
                run = 31 + src[s]
                s += 1
            dst.extend([src[s] + 128 * high] * run)
            s += 1
    return bytes(dst)


def pack_8to7_rle(src):
    """Port of pack_8to7_rle() in src/deluge/util/pack.c, used only by --selftest."""
    dst, s, i, n = [], 0, 0, len(src)

    def dense(chunk):
        hb = sum(1 << j for j, b in enumerate(chunk) if b & 0x80)
        off = {2: 0, 3: 4, 4: 12, 5: 28}[len(chunk)]
        return [off + hb] + [b & 0x7F for b in chunk]

    while s < n:
        k, val = s, src[s]
        s += 1
        while s < n and (s - k) < 158 and src[s] == val:
            s += 1
        dsz, rsz = k - i, s - k
        if rsz < 2:
            dsz += rsz
            rsz = 0
            if dsz < 5 and s < n:
                continue
        if dsz == 1:
            dst += [64 + 2 + (1 if src[i] & 0x80 else 0), src[i] & 0x7F]
        elif dsz > 0:
            dst += dense(src[i : i + dsz])
        if rsz > 0:
            first = 64 + (1 if val & 0x80 else 0)
            dst += [first + (rsz << 1)] if rsz < 31 else [first + 62, rsz - 31]
            dst.append(val & 0x7F)
        i = s
    return dst


def pack_8to7(src):
    """Port of pack_8bit_to_7bit() in src/deluge/util/pack.c (as task-loadfw.py packs the key)."""
    dst = []
    for i in range(0, len(src), 7):
        chunk = src[i : i + 7]
        dst.append(sum(1 << j for j, b in enumerate(chunk) if b & 0x80))
        dst += [b & 0x7F for b in chunk]
    return dst


def oled_ascii(img):
    """768 bytes = 6 pages x 128 columns, bit n of a byte = row 8*page+n (see hid_sysex.cpp, DelugeWeb app.js)."""
    rows = []
    for y in range(48):
        page, bit = divmod(y, 8)
        rows.append(
            "".join(
                "#" if img[page * 128 + x] & (1 << bit) else "." for x in range(128)
            )
        )
    return "\n".join(rows)


# ---- Links: whole sysex messages in and out ----


class RtMidiLink:
    """The last CoreMIDI port named like "Deluge" (port 3 is the one the dbt tools use for sysex).

    Real-time MIDI (clock, Start, Continue, Stop, song position) goes out on the first Deluge port instead (Port 1):
    the firmware builds upstream USB port 3 with clock input off (upstreamUSBMIDICable3 in midi_device_manager.cpp)
    and the MIDI DEVICES menu doesn't list it, so clock and transport sent to Port 3 are dropped. Port 1's CLOCK IN is
    on by default.
    """

    def __init__(self, quiet=False):
        import rtmidi

        self.mi, self.mo = rtmidi.MidiIn(), rtmidi.MidiOut()
        ins, outs = self.mi.get_ports(), self.mo.get_ports()
        if not quiet:
            print("MIDI in :", ins)
            print("MIDI out:", outs)
        di = [i for i, p in enumerate(ins) if "deluge" in p.lower()]
        do = [i for i, p in enumerate(outs) if "deluge" in p.lower()]
        if not di or not do:
            raise LookupError("No Deluge MIDI port. Nothing sent.")
        self.mi.ignore_types(sysex=False, timing=True, active_sense=True)
        self.mi.open_port(di[-1])
        self.mo.open_port(do[-1])
        self.clock_out = self.mo
        if do[0] != do[-1]:
            self.clock_out = rtmidi.MidiOut()
            self.clock_out.open_port(do[0])
        if not quiet:
            print(
                "using",
                ins[di[-1]],
                "/",
                outs[do[-1]],
                "; real-time MIDI to",
                outs[do[0]],
            )

    def close(self):
        self.mi.close_port()
        self.mo.close_port()
        if self.clock_out is not self.mo:
            self.clock_out.close_port()

    def send(self, msg):
        self.mo.send_message(list(msg))

    def send_midi(self, msg):
        """A MIDI message from a script: real-time and song position to Port 1, the rest to the test port."""
        if msg and msg[0] in REALTIME_TO_PORT_1:
            self.clock_out.send_message(list(msg))
        else:
            self.send(msg)

    def receive(self, timeout):
        """The next sysex message, or None after `timeout` seconds."""
        end = time.time() + timeout
        while True:
            m = self.mi.get_message()
            if m:
                if m[0][:1] == [0xF0]:
                    return m[0]
                continue
            if time.time() >= end:
                return None
            time.sleep(0.002)


class UdpLink:
    """delugemu's MIDI bridge: raw MIDI bytes in UDP datagrams."""

    def __init__(self, spec):
        listen, send = (int(p) for p in spec.split(":"))
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", listen))
        self.addr = ("127.0.0.1", send)
        self.buf = bytearray()

    def send(self, msg):
        self.sock.sendto(bytes(msg), self.addr)

    def send_midi(self, msg):
        self.send(msg)  # delugemu's DIN port takes clock and transport

    def close(self):
        self.sock.close()

    def receive(self, timeout):
        end = time.time() + timeout
        while True:
            start = self.buf.find(0xF0)
            stop = self.buf.find(0xF7, start) if start >= 0 else -1
            if stop >= 0:
                msg = list(self.buf[start : stop + 1])
                del self.buf[: stop + 1]
                return msg
            if start < 0:
                self.buf.clear()
            left = end - time.time()
            if left <= 0:
                return None
            self.sock.settimeout(left)
            try:
                data, _ = self.sock.recvfrom(65536)
            except TimeoutError:
                return None
            # Real-time bytes (clock and so on) may sit inside a sysex message on DIN.
            self.buf += bytes(b for b in data if b < 0xF8)


# ---- Test port client ----


class InjectRejected(RuntimeError):
    """The firmware refused an inject line: `status` is the reply's code (see STATUS)."""

    def __init__(self, line, status):
        super().__init__(f"inject {line!r}: {STATUS.get(status, status)}")
        self.status = status


IDLE_TIMEOUT = 10.0  # s an expect or a snapshot waits for the queued events by default


class TestPort:
    """Client for the ENABLE_SYSEX_TEST port. Collects event and log lines while waiting for replies."""

    def __init__(self, link, key=None):
        self.link = link
        self.key = [] if key is None else [0x00] + pack_8to7(key.to_bytes(4, "little"))
        self.seq = 0
        self.events, self.log = [], []

    def _next_seq(self):
        self.seq = (self.seq + 1) & 0x7F
        return self.seq

    def _collect(self, msg):
        if msg[:8] == EVENT_HDR:
            lines = self.events
        elif msg[:8] == LOG_HDR:
            lines = self.log
        else:
            return False
        text = bytes(msg[8:-1]).decode("ascii", "replace")
        lines += [line for line in text.splitlines() if line.strip()]
        return True

    def receive_until(self, match, timeout):
        end = time.time() + timeout
        while time.time() < end:
            msg = self.link.receive(end - time.time())
            if msg is None:
                break
            if not self._collect(msg) and match(msg):
                return msg
        return None

    def pump(self, seconds):
        self.receive_until(lambda m: False, seconds)

    def poll(self, fetch, done, timeout, step=0.1, retry_no_reply=False):
        """Calls fetch() until done(result) or `timeout` seconds pass, pumping in between. Returns the last result.

        With retry_no_reply, a fetch that gets no reply is retried until then: a song load blocks the test port for
        its whole length. Only for reads; a retried inject could queue its event twice.
        """
        end = time.time() + timeout
        while True:
            try:
                result = fetch()
            except TimeoutError:
                if not retry_no_reply or time.time() > end:
                    raise
                continue
            if done(result) or time.time() > end:
                return result
            self.pump(step)

    def _request(self, command, payload, reply_codes, timeout=2.0):
        seq = self._next_seq()
        self.link.send(HDR + [0x02, command, seq] + self.key + list(payload) + [0xF7])
        msg = self.receive_until(
            lambda m: m[:6] == HDR + [0x02] and m[6] in reply_codes and m[7] == seq,
            timeout,
        )
        if msg is None:
            raise TimeoutError(f"no reply to test port command {command:#x}")
        return msg

    def inject(self, line, timeout=10.0):
        """Queues one event line. Waits while the queue is full; raises on any other error."""
        msg = self.poll(
            lambda: self._request(INJECT, line.encode("ascii"), (STATUS_REPLY,)),
            lambda m: m[8] != QUEUE_FULL,
            timeout,
            0.05,
        )
        if msg[8] != QUEUED:
            raise InjectRejected(line, msg[8])

    def state(self):
        msg = self._request(STATE, [], (STATUS_REPLY, STATE_REPLY))
        if msg[6] == STATUS_REPLY:
            raise RuntimeError("state: " + STATUS.get(msg[8], str(msg[8])))
        return json.loads(bytes(msg[8:-1]).decode("ascii"))

    def wait_idle(self, timeout=None, sent=False):
        """Waits up to `timeout` seconds (IDLE_TIMEOUT if None) until the firmware has taken every queued event, and
        with `sent` also until it has sent every test event it queued (state's event_queue; a build older than that
        key isn't waited for). Returns the last state."""
        return self.poll(
            self.state,
            lambda s: (
                s["inject_queue"] == 0 and (not sent or s.get("event_queue", 0) == 0)
            ),
            IDLE_TIMEOUT if timeout is None else timeout,
            0.05,
            retry_no_reply=True,
        )

    def wait_sent(self, timeout=None):
        """wait_idle() with `sent`."""
        return self.wait_idle(timeout, sent=True)

    def oled(self):
        self.link.send(OLED_RLE)
        msg = self.receive_until(lambda m: m[:8] == OLED_REPLY_HDR, 2.0)
        return unpack_7to8_rle(msg[9:-1]) if msg else None

    def subscribe(self, on=True):
        self.link.send(LOG_ON if on else LOG_OFF)


def booted(state):
    """Whether the firmware has finished booting: a root UI is up and the card is idle (the boot song has loaded)."""
    return (
        state["root_ui"] != "none"
        and not state["sd_routine_lock"]
        and not state["accessing_card"]
    )


def link_failed(exc):
    """Whether `exc` is the link not answering or going away: no Deluge port, no reply, a socket error, or one of
    rtmidi's errors while a USB port disappears. rtmidi's classes live in its `_rtmidi` module and some don't derive
    from OSError, so they're matched by their base class's name (which also works without rtmidi installed)."""
    return isinstance(exc, (LookupError, TimeoutError, OSError)) or any(
        c.__name__ == "RtMidiError" for c in type(exc).__mro__
    )


def _link_gone(exc):
    """Whether a script's error means the link is gone, so the end-of-script release can't be sent. A LookupError
    only means that while opening the link (no Deluge port); inside a script it's a probe bug, and the release must
    still go out, or a "hold entropy" stays set."""
    return link_failed(exc) and not isinstance(exc, LookupError)


def wait_ready(open_link, key, timeout):
    """Waits until the test port answers and the firmware has booted. Returns the TestPort.

    Opens the link with open_link() and, while there's no answer, closes and reopens it: after loadfw the Deluge
    restarts, its USB MIDI ports go away and come back as new ports.
    """
    deadline = time.monotonic() + timeout
    while True:
        link = None
        try:
            link = open_link()
            port = TestPort(link, key)
            if booted(port.state()):
                return port
        except Exception as exc:
            if not link_failed(exc):
                raise
        if link is not None and hasattr(link, "close"):
            link.close()
        if time.monotonic() > deadline:
            raise TimeoutError(f"the test port did not answer within {timeout:g} s")
        time.sleep(0.5)


def _matches(value, expected):
    if isinstance(value, bool):
        return str(value).lower() == expected.lower()
    if isinstance(value, (int, float)):
        try:
            return value == float(expected)
        except ValueError:
            return False
    return str(value) == expected


# A (?P=name) or (?P<name>, not escaped with a backslash.
BACKREFERENCE = re.compile(r"(?<!\\)\(\?P=(\w+)\)")
NAMED_GROUP = re.compile(r"(?<!\\)\(\?P<(\w+)>")


# In `captured`, a name whose expect failed: no value, but a later (?P=name) fails its expect, not the script.
MISSING = object()


def _is_pattern(text):
    return len(text) > 1 and text.startswith("/") and text.endswith("/")


def _missing_capture(text, captured):
    """The first name a (?P=name) in TEXT reads whose capturing expect failed (MISSING), or None."""
    if not _is_pattern(text):
        return None
    body = text[1:-1]
    own = set(NAMED_GROUP.findall(body))
    return next(
        (
            name
            for name in BACKREFERENCE.findall(body)
            if name not in own and captured.get(name) is MISSING
        ),
        None,
    )


def _event_pattern(text, where, captured):
    """TEXT's regular expression, or None when TEXT isn't between slashes. A (?P=name) for a group the pattern doesn't
    have itself matches what an earlier pattern's group of that name captured, as literal text."""
    if not _is_pattern(text):
        return None
    body = text[1:-1]
    own = set(NAMED_GROUP.findall(body))

    def earlier(match):
        name = match.group(1)
        if name in own:
            return match.group(0)
        if name not in captured:
            raise ValueError(
                f"{where}: (?P={name}) with no {name} captured since start_state"
            )
        if captured[name] is MISSING:
            raise ValueError(
                f"{where}: (?P={name}): the expect that captures {name} failed"
            )
        return re.escape(captured[name])

    try:
        return re.compile(BACKREFERENCE.sub(earlier, body))
    except re.error as exc:
        raise ValueError(f"{where}: bad pattern {text!r}: {exc}") from None


def _event_matcher(text, pattern):
    """What an expect's TEXT matches: events containing it, or with TEXT between slashes, events whose text after the
    timestamp `pattern` (_event_pattern()'s) finds."""
    if pattern is not None:
        return lambda line: (
            pattern.search(EVENT_TIME.sub("", line, count=1)) is not None
        )
    return lambda line: text in line


def _capture(pattern, captured, line):
    """Keeps what the named groups of `pattern` (None: TEXT wasn't one) matched in `line`, the event an expect found."""
    match = pattern.search(EVENT_TIME.sub("", line, count=1)) if pattern else None
    if match is not None:
        captured.update({k: v for k, v in match.groupdict().items() if v is not None})


def _capture_failed(pattern, captured):
    """Marks the named groups of `pattern`, a failed expect's, that nothing has captured yet as MISSING, so an expect
    that reads one fails, naming it, and the script goes on."""
    for name in pattern.groupindex if pattern else ():
        captured.setdefault(name, MISSING)


def _expect_event(port, matches, since, timeout):
    """The index after the first event past `since` that `matches`, or None if none arrives in time."""

    def find():
        return next(
            (i + 1 for i in range(since, len(port.events)) if matches(port.events[i])),
            None,
        )

    return port.poll(find, lambda i: i is not None, timeout)


def _expect_next_event(port, kind, since, timeout):
    """The index after the first event of `kind` past `since`, or None if none arrives in time."""
    return _expect_event(port, lambda line: _event_kind(line) == kind, since, timeout)


RECENT_EVENTS = 5  # how many events of its kind a failed "expect event" prints
# The firmware's "%.4f: " before each event (TestPort::event).
EVENT_TIME = re.compile(r"^\d+(?:\.\d+)?: ")


def _event_kind(line):
    """An event's first word, after its timestamp."""
    return EVENT_TIME.sub("", line, count=1).split(" ", 1)[0]


# A pattern's kind: a plain word at its start, after an optional "^", that ends at a space or the pattern's end.
PATTERN_KIND = re.compile(r"/\^?([A-Za-z_]\w*)(?: |\$?/$)")


def _text_kind(text):
    """The event kind an expect's TEXT looks at: its first word, or the word a pattern starts with ("/^gen_ext_tempo
    bpm=12[01]$/" looks at gen_ext_tempo events). None for a pattern that doesn't start with a plain word
    ("/^\\s*gen_edit/", "/gen_left|gen_end/"): its kind isn't known."""
    if text.startswith("/"):
        word = PATTERN_KIND.match(text)
        return word.group(1) if word else None
    return text.split()[0]


# What a pattern with no kind looks for: its first word outside PATTERN_SYNTAX.
PATTERN_WORD = re.compile(r"[A-Za-z_]\w*")
# The parts of a pattern whose letters aren't words it looks for: a character class ("[a-z]"), a group's opening
# ("(?P<name>", "(?P=name)", "(?:", "(?i)") and an escape ("\s").
PATTERN_SYNTAX = re.compile(
    r"\[\^?\]?(?:\\.|[^\]\\])*\]|\(\?(?:P<\w+>|P=\w+\)|\w*)|\\."
)


def _recent_of_kind(port, text, since, searched_from, matched_where):
    """Why an "expect event TEXT" failed: the last RECENT_EVENTS events from `since` on whose first word is TEXT's,
    one "got ..." line each. TEXT may match inside an event ("left" matches gen_left), so with no event of that kind,
    it's the events containing TEXT's first word. A pattern with no kind ("/gen_left=|gen_end/") lists those of its
    first word (gen_left). With none, the line names what it looked for. The one at `searched_from - 1` is what the
    expect at `matched_where` matched, and those before it were passed by that expect."""
    kind = _text_kind(text)
    if kind is None:
        word = PATTERN_WORD.search(PATTERN_SYNTAX.sub(" ", text[1:-1]))
        kind, looked_for = (word.group(0) if word else text), text
    else:
        looked_for = kind
    events = list(enumerate(port.events[since:], since))
    found = [(i, line) for i, line in events if _event_kind(line) == kind] or [
        (i, line) for i, line in events if kind in EVENT_TIME.sub("", line, count=1)
    ]
    if not found:
        return [f"  got no {looked_for} event"]
    return [
        f"  got {line}" + _match_label(i, searched_from, matched_where)
        for i, line in found[-RECENT_EVENTS:]
    ]


def _match_label(i, searched_from, matched_where):
    if i == searched_from - 1 and matched_where is not None:
        return f" ({matched_where}'s match)"
    if i < searched_from:
        return f" (before {matched_where}'s match)"
    return ""


def _expect_oled_changed(port, frame, timeout):
    """Whether the OLED comes to differ from `frame` within `timeout` seconds."""

    def changed(current):
        return current is not None and current != frame

    return frame is not None and changed(port.poll(port.oled, changed, timeout))


DEFAULT_TIMEOUT = 5.0
LAST_LOOK = 0.2  # s an expect still looks for its match when waiting for the queue took its whole within=


def _options(args, where, default=DEFAULT_TIMEOUT):
    """Splits the trailing options "within=SECONDS" and "since=mark", in either order, off `args`. Returns (args,
    timeout, since_mark). A bare trailing number stays in `args`: it's part of the match."""
    timeout, since_mark = default, False
    args = list(args)
    while args and (args[-1].startswith("within=") or args[-1] == "since=mark"):
        arg = args.pop()
        if arg == "since=mark":
            since_mark = True
            continue
        try:
            timeout = float(arg[len("within=") :])
        except ValueError:
            raise ValueError(f"{where}: bad timeout {arg!r}") from None
    for arg in args:
        if arg.startswith(("within=", "since=")):
            raise ValueError(f"{where}: {arg.split('=')[0]}= must come last")
    return args, timeout, since_mark


def _expect_state(port, state, key, expected, timeout):
    """Polls until state[key] matches `expected`, starting from `state`. Returns (ok, last value)."""

    def match(s):
        return _matches(s.get(key), expected)

    if not match(state):
        state = port.poll(port.state, match, timeout, retry_no_reply=True)
    return match(state), state.get(key)


def read_script(path, depth=0):
    """A script file's lines as (where, text) pairs, with each "include FILE" line replaced by that file's lines.

    FILE is relative to the including script's directory. `where` is "line N" in the top file, "FILE:N" in an
    included one.
    """
    if depth > 8:
        raise ValueError(f"{path}: includes nested too deep")
    pairs = []
    with open(path) as fh:
        for number, raw in enumerate(fh, 1):
            word, _, rest = raw.split("#", 1)[0].strip().partition(" ")
            if word == "include":
                included = os.path.join(os.path.dirname(path), rest.strip())
                if not os.path.isfile(included):
                    raise ValueError(
                        f"{path}:{number}: include names a missing file {included!r}"
                    )
                for where, text in read_script(included, depth + 1):
                    name = os.path.basename(included)
                    if where.startswith("line "):
                        where = name + ":" + where[len("line ") :]
                    pairs.append((where, text))
            else:
                pairs.append((f"line {number}", raw))
    return pairs


class MidiClock:
    """Sends MIDI clock (0xF8, 24 a quarter note) from a background thread until stopped."""

    def __init__(self, link):
        self.link = link
        self.thread = None
        self.stopping = threading.Event()

    def start(self, bpm):
        self.stop()
        self.stopping.clear()
        period = 60.0 / (bpm * 24)

        def run():
            due = time.monotonic()
            while not self.stopping.is_set():
                self.link.send_midi([0xF8])
                due += period
                self.stopping.wait(max(0.0, due - time.monotonic()))

        self.thread = threading.Thread(target=run, daemon=True)
        self.thread.start()

    def stop(self):
        if self.thread is not None:
            self.stopping.set()
            self.thread.join()
            self.thread = None


START_STATE_STEPS = (
    12  # each closes one menu, stops PLAY, leaves Perform or presses CLIP
)


NEEDS = re.compile(r"^# needs:(.*)$", re.MULTILINE)
# "# needs: card DIR (why)": the folder, relative to the script's, that test_port_emu.py builds its card from
NEEDS_CARD = re.compile(r"^ card (\S+)")
ScriptNeeds = collections.namedtuple("ScriptNeeds", "delugemu card")


def script_needs(path, text=None):
    """What the script at `path` needs from where it runs, from its "# needs: ..." lines, as ScriptNeeds: whether
    delugemu can run it (no needs line, or only card ones), and the card overlay folder delugemu runs it on, or None
    for the factory card. `text` is the script's text, if the caller has read it already. Raises ValueError for a
    script with more than one card line."""
    if text is None:
        with open(path) as fh:
            text = fh.read()
    needs = NEEDS.findall(text)
    cards = [m.group(1) for m in map(NEEDS_CARD.match, needs) if m]
    if len(cards) > 1:
        raise ValueError(f'{path}: more than one "# needs: card" line: {cards}')
    card = os.path.join(os.path.dirname(path), *cards[0].split("/")) if cards else None
    return ScriptNeeds(len(cards) == len(needs), card)


def _press(port, name, hold_ms=50):
    port.inject(f"button {name} down")
    port.inject(f"wait {hold_ms}")
    port.inject(f"button {name} up")


LEAVE_SCRIPT = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "perform_leave.txt"
)


def _leave_perform(port):
    """Runs perform_leave.txt (hold BACK for LEAVE PERFORM?, then LEAVE, spec §13.8) quietly, stopping at its first
    failed expect: a SELECT push meant for LEAVE mustn't land on whatever else is open."""
    _run_lines(
        port, read_script(LEAVE_SCRIPT), lambda line: None, None, stop_on_fail=True
    )


# The holds of the builds older than "release all", which to_start_state() releases one by one there. Frozen: the
# firmware's kHolds table (test_port.cpp) is the list of holds, and a new one is not added here.
HOLDS_BEFORE_RELEASE_ALL = (
    "entry",
    "ramps",
    "stress",
    "card_error",
    "preload",
    "midi_fill",
    "midi_setup",
)


def _release_holds(port, warn):
    """Lets go of every hold a script may have left ("release all"; harmless when nothing is held): a script that
    stopped after a "hold" would leave the next one's entry waiting, its sound loads failing, and so on. A build
    older than "release all" rejects it as a parse error, and gets each of HOLDS_BEFORE_RELEASE_ALL released instead,
    with a warn() for each one it doesn't know either."""
    try:
        port.inject("release all")
        return
    except InjectRejected as exc:
        if exc.status != PARSE_ERROR:
            raise
    for held in HOLDS_BEFORE_RELEASE_ALL:
        try:
            port.inject(f"release {held}")
        except InjectRejected as exc:
            if exc.status != PARSE_ERROR:
                raise
            warn(
                f"the build doesn't know `release {held}` (older than `hold {held}`), went on without it: {exc}"
            )


# How long start_state waits for gen_recipe_reset: delugemu's DIN link delays events
RECIPE_RESET_TIMEOUT = 10.0


def _reset_recipe(port, warn, timeout=RECIPE_RESET_TIMEOUT):
    """Sends "reset recipe": the Current Recipe as after boot, so the next Perform entry opens GENRE on the default
    genre (techno-acid) and rolls a fresh Recipe whatever genre the last script left playing. Returns (ok, detail).
    A refusal for a runner job still running (why=busy, right after a leave) is retried until `timeout`. A build
    older than the command rejects it as a parse error: a warn(), and the Current Recipe stays as it was."""
    end = time.time() + timeout
    while True:
        since = len(port.events)
        try:
            port.inject("reset recipe")
        except InjectRejected as exc:
            if exc.status != PARSE_ERROR:
                raise
            warn(
                "the build doesn't know `reset recipe` (older than it), went on without it: the Current Recipe "
                f"stays as the last script left it: {exc}"
            )
            return True, ""
        # At least a second: a retry sent just before the deadline still gets its answer read.
        found = _expect_event(
            port,
            lambda line: "gen_recipe_reset " in line,
            since,
            max(end - time.time(), 1.0),
        )
        if found is None:
            return False, "no gen_recipe_reset event after `reset recipe`"
        result = EVENT_TIME.sub("", port.events[found - 1], count=1)
        if result == "gen_recipe_reset done":
            return True, ""
        if "why=busy" not in result or time.time() > end:
            return False, f"`reset recipe` got {result}"
        port.pump(0.3)


def to_start_state(port, steps=START_STATE_STEPS, warn=lambda text: None):
    """Puts the device where test scripts start, as after boot: transport stopped, in an instrument (synth or kit)
    clip, nothing open over it, and no Current Recipe. Returns (ok, detail).

    First it releases every hold a script may have left (see _release_holds(); on a build older than a hold that
    gets a warn() and the rest goes ahead, so current scripts still run on old builds when bisecting). Then each step
    looks at the state and does one thing: stop PLAY, close
    what's open over the root UI (BACK), leave Perform, or go into the current clip (CLIP from session, automation;
    KEYBOARD from the keyboard screen). Last, outside Perform, it resets the Current Recipe (see _reset_recipe(); a
    build older than that gets a warn() too).
    """
    detail = ""
    _release_holds(port, warn)
    for _ in range(steps):
        s = port.wait_idle()
        if not booted(s) or s["ui_mode"] != 0:
            port.pump(0.3)  # a song loading, or an animation
            s = port.state()
        ui, root = s["ui"], s["root_ui"]
        detail = f"(ui={ui} root_ui={root} playing={str(s['playing']).lower()})"
        if s["playing"]:
            _press(port, "PLAY")
            if port.wait_idle()["playing"]:
                port.link.send_midi([0xFC])  # following an external clock
        elif ui != root:
            _press(port, "BACK")
        elif root == "instrument_clip" and s["ui_mode"] == 0 and booted(s):
            return _reset_recipe(port, warn)
        elif root == "generative_perform":
            _leave_perform(port)
        elif root in ("session", "automation"):
            _press(port, "CLIP_VIEW")
        elif root == "arranger":
            _press(port, "SESSION_VIEW")
        elif root == "keyboard_screen":
            _press(port, "KEYBOARD")
        else:
            _press(port, "BACK")
        port.pump(0.3)
    return False, f"never got to a stopped instrument clip {detail}"


class ScriptCounts:
    """The expects a script has run so far, which _run_lines() counts into: run_script() still has them when an error
    (a hang) stops the script."""

    def __init__(self):
        self.failed = self.total = 0


# The errors that stop a script partway: a bad line (ValueError), a reply the probe doesn't expect (RuntimeError),
# no reply or a link gone (OSError, TimeoutError among them).
STOP_ERRORS = (RuntimeError, ValueError, OSError)


class ScriptStopped(RuntimeError):
    """An error that stopped a script partway (one of STOP_ERRORS: a bad line, or no reply from the test port, a
    hang), with the expects the script ran first. `error` is the error, also the __cause__. `ran` is False for a
    script that never started (it can't be read)."""

    def __init__(self, error, failed=0, total=0, ran=True):
        super().__init__(
            f"stopped after {total - failed} passed and {failed} failed: {error}"
            if ran
            else str(error)
        )
        self.error, self.failed, self.total, self.ran = error, failed, total, ran

    def result(self):
        """(failed, total, text) for result_line(): the stop counts as one more failed expect, and the text says how
        far the script got first. A script that never started has no expects: one failed, a total of 0 and the plain
        error."""
        if not self.ran:
            return 1, 0, str(self)
        return self.failed + 1, self.total + 1, str(self)


def run_script(port, lines, out=print, on_start_state=None):
    r"""Runs a test script against a TestPort. Returns (failed, total) expects. After each start_state, it calls
    on_start_state(index), if given, with the index into port.events after what that start_state caused.

    `lines` are strings, or (where, text) pairs from read_script(). One command per line; "#" starts a comment.
      include FILE                                 the lines of FILE, relative to the script (read_script() only)
      start_state                                  puts the device where scripts start: transport stopped, in an
                                                   instrument clip (leaves Perform, closes menus, stops PLAY, then
                                                   CLIP), then sends "reset recipe". Counts as an expect. Put it
                                                   first, so scripts run in any order on one load. It first sends
                                                   "release all"; on a build older than that it releases the holds
                                                   by name, and prints a WARNING line for each one the build doesn't
                                                   know, and one if it doesn't know "reset recipe"
      button ..., pad ..., encoder ..., wait ...   queued on the firmware (see parseLine() in test_port.cpp)
      button F1..F8 down|up                        a mod button by its panel name (button 1 1 is F2)
      encoder 4|5 DETENTS                          a gold knob: each detent 4 edges 0.16 s apart, each a one-step
                                                   edit, so 4 edits (4 gen_edit events) a detent, 0.64 s
      encoder 4|5 DETENTS fast                     the same steps, each detent's 4 at once as one edit (one
                                                   gen_edit): 32 detents in about a second
      hold NAME | release NAME                     queued too: sets one of the firmware's test hooks on or off. The
                                                   names and what each does: kHolds in test_port.cpp. "hold entropy"
                                                   pins Perform's rolls: the next entry rolls the same Recipe and part
                                                   presets on every run from the same card
      release all                                  queued too: sets every hold off. start_state sends it first
      reset recipe                                 queued too: the Current Recipe as after boot (event
                                                   gen_recipe_reset done, or refused why=perform|busy, nothing
                                                   changed). start_state sends it last
      pick genre NAME                              queued too: focuses genre NAME's row (techno-dub) in the open GENRE
                                                   menu, its parent's row in GENRE itself (event gen_genre_pick NAME
                                                   row=LABEL, or refused): pick a genre by name, not by counting rows
      report input                                 queued too: event input_lag with the longest press lag and gold-knob
                                                   edge gap since the last report, ok=yes within spec §12.10's bounds
      report boot_dma                              queued too: event boot_dma running=N mask=0xMMMM, then a
                                                   boot_dma_channel line for each DMA channel the previous firmware
                                                   left running when this one started (#166)
      load song PATH                               queued too: loads the song at PATH on the card as the browser
                                                   does (event song_loaded, or song_load_failed). Test builds only;
                                                   blocks the test port while it loads
      expect KEY=VALUE [within=S]                  polls the state JSON until KEY equals VALUE
      keep KEY...                                  keeps the state JSON's KEYs as they are once the queue is empty
                                                   (start_state drops them)
      expect KEY=kept [within=S]                   polls until KEY equals the value "keep" kept: "the song is
                                                   untouched" when the value isn't known ahead (undo_log, bpm)
      keep events TEXT                             keeps the events TEXT matches since the last "mark" (one is
                                                   needed), once the queue is empty (start_state drops them)
      expect events kept [within=S]                polls until as many events TEXT matches came since the last mark,
                                                   then passes if they are the kept ones, in order: "the same as
                                                   before" when the events aren't known ahead (the sounds an entry
                                                   loads depend on the card). A failure prints the first difference.
                                                   TEXT's (?P=name)s read the captures as "keep events" did
      expect oled_changed [within=S]               polls until the OLED differs from the last "snapshot"
      expect event TEXT [within=S]                 polls until a test event containing TEXT arrives, counting only
                                                   events after the one the previous "expect event" matched. On a
                                                   failure it prints the script's last events of TEXT's kind (same
                                                   first word after the timestamp) as "got ..." lines
                                                   TEXT between slashes is a regular expression, searched in the
                                                   event's text after its timestamp: /gen_ext_tempo bpm=1(19|2[01])$/
                                                   A named group keeps what it matched until start_state, and a later
                                                   pattern's (?P=name) matches that text: a value the script can't know
                                                   ahead, checked where it turns up again:
                                                   /gen_pending_recipe tempo=(?P<tempo>\d+)/, then /tempo=(?P=tempo)$/
                                                   If the expect with the group failed, one that reads it fails too
      expect event TEXT since=mark [within=S]      the same, counting from where the expect search stood at the last
                                                   "mark" instead: for events a frame may report in either order. The
                                                   next expect searches after the later of the two matches
      expect next event TEXT [since=mark] [within=S]
                                                   "this event and no other": the next event of TEXT's kind (its first
                                                   word) must match TEXT. It fails at once on one that doesn't, where
                                                   "expect event" would wait for a later match. A pattern must start
                                                   with the kind, a plain word, after an optional ^: /^gen_edit part=1/
      mark                                         keeps where the expect search stands (start_state drops it)
      expect no_event TEXT [within=S]              passes if no such event arrives for S seconds, counted the same way
      expect no_event TEXT since=mark [within=S]   the same, counting from the last "mark": also fails on one that
                                                   came before the previous expect's match
      midi HEX...                                  sends one MIDI message from the host, e.g. "midi 90 3c 64", once
                                                   the firmware has run every queued event
      clock BPM | clock off                        starts (or stops) sending MIDI clock from the host in the
                                                   background, 24 a quarter note, until "clock off" or the end
      snapshot                                     keeps the current OLED frame
      sleep SECONDS                                host-side pause
    After the last line, a script that ran start_state fails one more expect if the firmware dropped test events
    after it (state's events_dropped), up to the next start_state or the end, each once the firmware has sent what it
    queued (wait_sent()): a dropped event fails an expect that waited for it, or passes an expect no_event, for a
    reason the script doesn't test. With none dropped, nothing is printed or counted.
    Expects time out after 5 s unless they end in within=SECONDS. A timeout needs the keyword: a bare trailing number
    is part of TEXT ("expect event gen_peek 2" matches line 2), and makes a KEY=VALUE or oled_changed expect a script
    error. since=mark goes only with "event" and "no_event". Expects and snapshots first wait until the firmware has
    run every queued event, waits, encoder turns and song loads included: up to IDLE_TIMEOUT, or an expect's within=
    if longer (give the expect after a load one). within= counts from the expect line, that wait included, so a
    failing expect ends about S after it starts; "no_event" alone watches its whole S after the wait. When the wait
    takes all of S, the expect still looks for LAST_LOOK (0.2 s), and a failure says the queue took its window: an
    expect after long queued waits or turns needs a within= that covers them.

    On a Deluge, real-time MIDI (clock, Start, Stop, Continue, song position) goes to Deluge Port 1, the rest to the
    test port's Port 3: the firmware ignores clock and transport on Port 3 (see RtMidiLink).

    An error that stops the script (STOP_ERRORS: a bad line, or no reply, a hang) raises ScriptStopped from it, with
    the expects run before it.
    """
    clock = MidiClock(port.link)
    counts = ScriptCounts()
    lost_link = False
    try:
        return _run_lines(
            port, lines, out, clock, on_start_state=on_start_state, counts=counts
        )
    except STOP_ERRORS as exc:
        lost_link = _link_gone(exc)
        raise ScriptStopped(exc, counts.failed, counts.total) from exc
    except Exception as exc:
        lost_link = _link_gone(exc)
        raise
    finally:
        clock.stop()
        if not lost_link:
            _release_at_end(port)


def _release_at_end(port):
    """Lets go of the holds a script leaves, also when it stops on an error: "hold entropy" left on pins every later
    entry until a reboot. Not after a lost link (run_script()): a hung firmware answers nothing, and the release
    would only delay the hang's report by an inject's wait. Quiet: start_state warned about the holds an old build
    doesn't know, and a link lost now leaves the script's own error to tell."""
    # The script's own result or error matters more than a release that failed.
    with contextlib.suppress(Exception):
        _release_holds(port, warn=lambda text: None)


def events_pattern(text):
    """--events PATTERN as a compiled regex, or None without the option. Raises ValueError saying why PATTERN isn't a
    regex."""
    if text is None:
        return None
    try:
        return re.compile(text)
    except re.error as exc:
        raise ValueError(f"--events {text!r} isn't a regex: {exc}") from None


def script_events(events, seen, starts, pattern):
    """With --events: the test events a script caused that `pattern` matches. They count from after its first
    start_state (what that one caused, such as leaving Perform, isn't the script's; a script should have only one),
    or from `seen`, where `events` stood when it began, if it had none. `starts` are run_script()'s on_start_state
    indexes. Nothing without a pattern."""
    if pattern is None:
        return []
    return [
        line for line in events[starts[0] if starts else seen :] if pattern.search(line)
    ]


def result_line(name, failed, total, error=None):
    """A script's one-line result, as the hardware session and delugemu runs print it."""
    return f"{name}: {total} expects, {failed} failed" + (
        f" ({error})" if error else ""
    )


LAST_EVENTS = 10  # events of any kind a script that stopped on an error (a hang) prints, whatever --events says


def last_events(events, seen):
    """The lines a script that stopped on an error prints: the last LAST_EVENTS of `events` from `seen` on (where
    the script began), of any kind."""
    lines = events[seen:][-LAST_EVENTS:]
    if not lines:
        return ["no events since the script began"]
    return [
        f"the last {len(lines)} events before it stopped, of any kind:",
        *(f"last {line}" for line in lines),
    ]


def run_one_script(port, script, out=print, events=None):
    """Runs the script file `script` on `port`, as the hardware session and delugemu runs do. Its lines go to out(),
    then "event ..." for each test event the compiled regex `events` matches (script_events()). A script an error
    stops also prints its last events of any kind (last_events()). Returns (failed, total); raises ScriptStopped,
    also for a script that can't be read."""
    try:
        lines = read_script(script)
    except (ValueError, OSError) as exc:
        raise ScriptStopped(exc, ran=False) from exc
    seen = len(port.events)
    starts = []  # where port.events stood after each start_state
    stopped = True
    try:
        result = run_script(port, lines, out=out, on_start_state=starts.append)
        stopped = False
        return result
    finally:
        for line in script_events(port.events, seen, starts, events):
            out(f"event {line}")
        if stopped:
            for line in last_events(port.events, seen):
                out(line)


def _run_lines(
    port, lines, out, clock, stop_on_fail=False, on_start_state=None, counts=None
):
    counts = ScriptCounts() if counts is None else counts
    frame, events_seen = None, len(port.events)
    # Test events the firmware dropped after each start_state, and its events_dropped at the last one.
    dropped, dropped_from = 0, None
    # Where the script's own events start (after its start_state), and the line of the last "expect event" match.
    script_events_from, matched_where = events_seen, None
    mark = None  # where "mark" left the search for "expect event ... since=mark"
    # State values "keep" kept, for "expect KEY=kept"; "events": (TEXT, its pattern, events) "keep events" kept. The
    # pattern is kept too, so "expect events kept" reads the captures as "keep events" did.
    kept = {}
    captured = {}  # what expect event patterns' named groups matched, for a later (?P=name)
    for number, raw in enumerate(lines, 1):
        if counts.failed and stop_on_fail:
            break
        where, raw = raw if isinstance(raw, tuple) else (f"line {number}", raw)
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        word, _, rest = line.partition(" ")
        if word in (
            "button",
            "pad",
            "encoder",
            "wait",
            "hold",
            "release",
            "load",
            "report",
            "reset",
            "pick",
        ):
            port.inject(line)
        elif word == "start_state":
            if dropped_from is not None:
                dropped += _dropped_since(dropped_from, _events_dropped(port))
            ok, detail = to_start_state(
                port,
                warn=lambda text, where=where: out(
                    f"WARNING {where}: start_state: {text}"
                ),
            )
            # What start_state caused isn't the script's, its drops included: so first its events are sent.
            dropped_from = _events_dropped(port)
            events_seen = len(port.events)
            script_events_from, matched_where, mark, kept = events_seen, None, None, {}
            captured = {}
            if on_start_state is not None:
                on_start_state(events_seen)
            out(
                "{} {}: start_state {}".format(
                    "PASS" if ok else "FAIL", where, detail
                ).rstrip()
            )
            counts.failed += 0 if ok else 1
            counts.total += 1
        elif word == "midi":
            port.wait_idle()
            port.link.send_midi([int(b, 16) for b in rest.split()])
        elif word == "clock":
            port.wait_idle()
            if rest == "off":
                clock.stop()
            else:
                clock.start(float(rest))
        elif word == "sleep":
            port.pump(float(rest))
        elif word == "mark":
            mark = events_seen
        elif word == "keep" and rest.split()[:1] == ["events"]:
            text = rest.partition(" ")[2].strip()
            if mark is None:
                raise ValueError(f"{where}: keep events with no mark since start_state")
            if not text:
                raise ValueError(f"{where}: bad keep {line!r}")
            port.wait_sent()  # what the firmware queued, sent, so the list is whole
            pattern = _event_pattern(text, where, captured)
            events = _events_since(port, text, pattern, mark)
            if not events:
                raise ValueError(
                    f"{where}: keep events kept no {text} events, so expect events kept would pass on none"
                )
            kept["events"] = (text, pattern, events)
        elif word == "keep" and rest.split():
            state = port.wait_idle()
            for key in rest.split():
                if key not in state:
                    raise ValueError(f"{where}: keep: no state key {key!r}")
                kept[key] = (
                    str(state[key]).lower()
                    if isinstance(state[key], bool)
                    else str(state[key])
                )
        elif word == "snapshot":
            port.wait_idle()
            frame = port.oled()
        elif word == "expect":
            what, _, rest = rest.partition(" ")
            args, timeout, since_mark = _options(rest.split(), where)
            next_only = what == "next"
            if next_only:
                if args[:1] != ["event"]:
                    raise ValueError(f"{where}: bad expect {line!r}")
                what, args = "event", args[1:]
            if since_mark and what not in ("event", "no_event"):
                raise ValueError(
                    f"{where}: since=mark only goes with expect event or no_event"
                )
            if since_mark and mark is None:
                raise ValueError(f"{where}: since=mark with no mark since start_state")
            started = time.time()
            # A long within= covers a slow queued entry too: a song load blocks the test port while it runs.
            idle = port.wait_idle(max(IDLE_TIMEOUT, timeout))
            # within= counts from the expect line, the queue's wait included, so a failing expect takes about its
            # within=; when the queue took all of it, the match still gets a short look.
            waited = time.time() - started
            left = max(timeout - waited, LAST_LOOK)
            detail, got_lines = "", []
            missing = (
                _missing_capture(" ".join(args), captured)
                if what in ("event", "no_event")
                else None
            )
            if missing is not None:
                ok = False
                detail = f"(no {missing} captured: the expect that captures it failed)"
            elif what == "oled_changed" and not args:
                ok = _expect_oled_changed(port, frame, left)
            elif what == "event" and args:
                text = " ".join(args)
                since = mark if since_mark else events_seen
                pattern = _event_pattern(text, where, captured)
                matches = _event_matcher(text, pattern)
                if next_only:
                    kind = _text_kind(text)
                    if kind is None:
                        raise ValueError(
                            f"{where}: expect next event {text}: the pattern must start with its event's kind, a"
                            " plain word (/^gen_edit part=1/)"
                        )
                    found = _expect_next_event(port, kind, since, left)
                    ok = found is not None and matches(port.events[found - 1])
                else:
                    found = _expect_event(port, matches, since, left)
                    ok = found is not None
                if ok:
                    _capture(pattern, captured, port.events[found - 1])
                    # A match since the mark can lie before the last one: the next expect searches after both.
                    if found >= events_seen:
                        matched_where = where
                    events_seen = max(events_seen, found)
                else:
                    _capture_failed(pattern, captured)
                    got_lines = (
                        [f"  got {port.events[found - 1]}"]
                        if found is not None
                        else _recent_of_kind(
                            port, text, script_events_from, since, matched_where
                        )
                    )
            elif what == "no_event" and args:
                text = " ".join(args)
                found = _expect_event(
                    port,
                    _event_matcher(text, _event_pattern(text, where, captured)),
                    mark if since_mark else events_seen,
                    timeout,
                )
                ok = found is None
                if not ok:
                    got_lines = [f"  got {port.events[found - 1]}"]
            elif what == "events" and args == ["kept"]:
                if "events" not in kept:
                    raise ValueError(
                        f"{where}: expect events kept with no keep events since start_state"
                    )
                text, pattern, wanted = kept["events"]
                got = _expect_kept_events(port, text, pattern, wanted, mark, left)
                ok = got == wanted
                if not ok:
                    got_lines = _first_difference(text, wanted, got)
            elif "=" in what and not args:
                key, _, expected = what.partition("=")
                if expected == "kept":
                    if key not in kept:
                        raise ValueError(
                            f"{where}: {key}=kept with no keep {key} since start_state"
                        )
                    expected = kept[key]
                ok, value = _expect_state(port, idle, key, expected, left)
                detail = f"(got {value!r})"
            else:
                raise ValueError(f"{where}: bad expect {line!r}")
            # The queue took more than the last look and left the expect no more than it.
            if (
                not ok
                and missing is None
                and what != "no_event"
                and LAST_LOOK < waited
                and timeout <= waited + LAST_LOOK
            ):
                got_lines.append(
                    f"  the queue before it took {waited:.1f} s of its within={timeout:g}: the expect got only a"
                    " last look"
                )
            out(
                "{} {}: {} {}".format(
                    "PASS" if ok else "FAIL", where, line, "" if ok else detail
                ).rstrip()
            )
            for got in got_lines:
                out(got)
            counts.failed += 0 if ok else 1
            counts.total += 1
        else:
            raise ValueError(f"{where}: unknown command {word!r}")
    if dropped_from is not None:
        dropped += _dropped_since(dropped_from, _events_dropped(port))
    if dropped > 0:
        out(
            f"FAIL end: {dropped} test events dropped since start_state: the firmware's event queue was full, so an"
            " expect may have failed, or an expect no_event passed, for want of its event (test_events_dropped"
            " marks the place in the log)"
        )
        counts.failed += 1
        counts.total += 1
    return counts.failed, counts.total


def _events_since(port, text, pattern, since):
    """The events from `since` on that TEXT (`pattern`, _event_pattern()'s) matches, each without its timestamp."""
    matches = _event_matcher(text, pattern)
    return [
        EVENT_TIME.sub("", line, count=1)
        for line in port.events[since:]
        if matches(line)
    ]


def _expect_kept_events(port, text, pattern, wanted, since, timeout):
    """The events from `since` on that TEXT (`pattern`) matches, once there are as many as `wanted` or `timeout`
    passes."""
    return port.poll(
        lambda: _events_since(port, text, pattern, since),
        lambda events: len(events) >= len(wanted),
        timeout,
    )


def _first_difference(text, wanted, got):
    """Why an "expect events kept" failed: how many events each side has, and the first pair that differs."""
    at = next(
        (i for i, (a, b) in enumerate(zip(wanted, got)) if a != b),
        min(len(wanted), len(got)),
    )
    return [
        f"  kept {len(wanted)} {text} events, got {len(got)}; the first that differs:",
        f"  kept {wanted[at]}" if at < len(wanted) else "  kept none",
        f"  got {got[at]}" if at < len(got) else "  got none",
    ]


def _dropped_since(before, now):
    """Test events dropped between two readings of state's events_dropped. It counts from boot, so a fall is a
    firmware reset, and the new value is all dropped since."""
    return now - before if now >= before else now


def _events_dropped(port):
    """Test events the firmware dropped since it booted, once it has sent what it queued: a burst can fill its queue
    (TestPort::event()), and a line dropped while the queue empties belongs to what came before."""
    state = port.wait_sent()
    if state.get("event_queue", 0) or state.get("inject_queue", 0):
        print(
            "WARNING: the firmware still had lines to send when the wait gave up, so drops may be counted against"
            " the wrong part of the script"
        )
    return state.get("events_dropped", 0)


# ---- Commands ----


def probe(port, log_seconds):
    link = port.link
    link.send(IDENTITY_REQ)
    link.send(PING)
    end = time.time() + 1.0
    while (m := link.receive(end - time.time())) is not None:
        if m[:5] == [0xF0, 0x7E, 0x7F, 0x06, 0x02]:
            print("identity reply, firmware {}.{}.{}".format(*m[12:15]), m)
        elif m[:6] == HDR + [0x7F]:
            print("pong", m)

    img = port.oled()
    if img is not None:
        print(f"OLED ({len(img)} bytes):")
        print(oled_ascii(img))

    if log_seconds > 0:
        stream(port, log_seconds, events=False, log=True)


def stream(port, seconds, events, log):
    """Subscribes to the debug log and prints event and/or log lines as they arrive."""
    port.subscribe()
    try:
        end = time.time() + seconds
        while time.time() < end:
            port.pump(min(0.2, end - time.time()))
            for line in port.events if events else []:
                print("event", line)
            for line in port.log if log else []:
                print("log  ", line)
            port.events.clear()
            port.log.clear()
    finally:
        port.subscribe(False)


def read_key(args):
    if args.key:
        return int(args.key, 16)
    if args.key_file:
        with open(args.key_file) as fh:
            return int(fh.read().strip(), 16)
    return None


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument(
        "command",
        nargs="?",
        default="probe",
        choices=("probe", "state", "inject", "events", "run", "ready", "session"),
    )
    ap.add_argument(
        "args",
        nargs="*",
        help="inject: event lines; run: the script file; session: script files",
    )
    ap.add_argument(
        "--build",
        help="session: the build to load, e.g. release (./dbt loadfw BUILD)",
    )
    ap.add_argument(
        "--no-load", action="store_true", help="session: use the running build"
    )
    ap.add_argument(
        "--wait",
        type=float,
        default=1800,
        help="session: how long to wait for the lock, in seconds",
    )
    ap.add_argument(
        "--no-wait",
        action="store_true",
        help="session: fail at once if the lock is held",
    )
    ap.add_argument(
        "--owner", help="session: who holds the lock and why (e.g. a ticket)"
    )
    ap.add_argument(
        "--note", help="session: what else passing looks like, for a pending.md entry"
    )
    ap.add_argument(
        "--events",
        metavar="PATTERN",
        help="session: after each script, print its test events matching the regex PATTERN (. for all of them)",
    )
    ap.add_argument(
        "--clear-hung",
        action="store_true",
        help="session: move the hung marker away, only after the user has power cycled",
    )
    ap.add_argument(
        "--log-seconds",
        type=float,
        default=0,
        help="probe: also print the debug log this long",
    )
    ap.add_argument(
        "--seconds", type=float, default=10, help="events: how long to listen"
    )
    ap.add_argument(
        "--log", action="store_true", help="events, run: also print debug log lines"
    )
    ap.add_argument(
        "--timeout",
        type=float,
        default=90,
        help="ready, session: how long to wait for the test port, in seconds",
    )
    ap.add_argument(
        "--udp",
        metavar="LISTEN:SEND",
        help="delugemu MIDI bridge ports instead of CoreMIDI",
    )
    ap.add_argument("--key", help="dev-sysex key (hex) to send with test port requests")
    ap.add_argument(
        "--key-file", help="read the key from this file, e.g. .deluge_hex_key"
    )
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    # A session's output often goes to a file someone polls: send each line as it's printed.
    sys.stdout.reconfigure(line_buffering=True)

    if a.selftest:
        rnd = random.Random(1)
        for _ in range(200):
            data = bytes(
                rnd.choice([0, 0, 0, 255, rnd.randrange(256)]) for _ in range(768)
            )
            assert unpack_7to8_rle(pack_8to7_rle(data)) == data
        assert pack_8to7((0x89ABCDEF).to_bytes(4, "little")) == [
            0x0F,
            0x6F,
            0x4D,
            0x2B,
            0x09,
        ]
        print("selftest: RLE round-trip ok (200 x 768 bytes), key packing ok")

    if a.command == "session":
        import hardware_session

        return hardware_session.main(sys.modules[__name__], a)

    if a.command == "ready":
        try:
            port = wait_ready(
                (lambda: UdpLink(a.udp)) if a.udp else (lambda: RtMidiLink(quiet=True)),
                read_key(a),
                a.timeout,
            )
        except TimeoutError as exc:
            print(exc)
            return 1
        print(json.dumps(port.state(), indent=2))
        return 0

    try:
        link = UdpLink(a.udp) if a.udp else RtMidiLink()
    except LookupError as exc:
        print(exc)
        return 0 if a.command == "probe" else 1

    port = TestPort(link, read_key(a))
    if a.command == "probe":
        probe(port, a.log_seconds)
        return 0

    try:
        return test_port_command(ap, a, port)
    except (TimeoutError, RuntimeError) as exc:
        print(exc)
        return 1


def test_port_command(ap, a, port):
    if a.command == "state":
        print(json.dumps(port.state(), indent=2))
    elif a.command == "inject":
        for line in a.args:
            port.inject(line)
            print("queued", line)
    elif a.command == "events":
        stream(port, a.seconds, events=True, log=a.log)
    elif a.command == "run":
        if len(a.args) != 1:
            ap.error("run takes one script file")
        lines = read_script(a.args[0])
        port.subscribe()
        try:
            failed, total = run_script(port, lines)
        finally:
            port.subscribe(False)
        if a.log:
            print("\n".join(port.log))
        print(f"{total} expects, {failed} failed")
        return 1 if failed else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
