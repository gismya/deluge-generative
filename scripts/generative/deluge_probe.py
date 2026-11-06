"""Deluge USB-MIDI probe and sysex test port client.

Commands:
    probe            (default) read-only: identity, ping, one OLED dump, optionally the debug log
    state            print the test port's state JSON
    inject LINE...   queue input events, e.g. "button SHIFT down", "pad 3 4 down", "encoder 5 2", "wait 100"
    events           print test events (and with --log, debug log lines) for --seconds
    run SCRIPT       run a test script (see run_script), exit 1 if an expect fails
    ready            wait until the test port answers and the song has loaded, e.g. after loadfw (--timeout)

state, inject, events, run and ready need a build configured with -DENABLE_SYSEX_TEST=ON (see
src/deluge/io/midi/test_port.h). The probe sends nothing to ports whose name doesn't contain "deluge".

--udp LISTEN:SEND talks to delugemu's MIDI bridge instead of CoreMIDI, as raw MIDI bytes on 127.0.0.1: launch the
emulator with --midi udp:127.0.0.1:LISTEN@127.0.0.1:SEND. Use the DIN port: delugemu's USB-MIDI model never delivers
host input to the firmware.

Run with the dbt toolchain python (has python-rtmidi 1.5.8):
    ./dbt exec "python3 scripts/generative/deluge_probe.py [--log-seconds N] [--selftest]"
    ./dbt exec "python3 scripts/generative/deluge_probe.py run scripts/generative/test_port_smoke.txt"
See docs/hardware-measurements/deluge-usb-control.md on task/hardware-measurements.
"""

import argparse
import json
import os
import random
import socket
import sys
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
STATUS = {0: "queued", 1: "queue full", 2: "parse error", 3: "bad key"}
LOG_HDR = HDR + [0x03, 0x40, 0x00]
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
    """The last CoreMIDI port named like "Deluge" (port 3 is the one the dbt tools use for sysex)."""

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
        if not quiet:
            print("using", ins[di[-1]], "/", outs[do[-1]])

    def close(self):
        self.mi.close_port()
        self.mo.close_port()

    def send(self, msg):
        self.mo.send_message(list(msg))

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

    def poll(self, fetch, done, timeout, step=0.1):
        """Calls fetch() until done(result) or `timeout` seconds pass, pumping in between. Returns the last result."""
        end = time.time() + timeout
        while True:
            result = fetch()
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
            lambda m: m[8] != 1,  # not "queue full"
            timeout,
            0.05,
        )
        if msg[8] != 0:
            raise RuntimeError(f"inject {line!r}: {STATUS.get(msg[8], msg[8])}")

    def state(self):
        msg = self._request(STATE, [], (STATUS_REPLY, STATE_REPLY))
        if msg[6] == STATUS_REPLY:
            raise RuntimeError("state: " + STATUS.get(msg[8], str(msg[8])))
        return json.loads(bytes(msg[8:-1]).decode("ascii"))

    def wait_idle(self, timeout=10.0):
        """Waits until the firmware has taken every queued event. Returns the last state."""
        return self.poll(self.state, lambda s: s["inject_queue"] == 0, timeout, 0.05)

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
        except (LookupError, TimeoutError, OSError):
            pass
        except (
            Exception
        ) as exc:  # rtmidi raises its own errors while a port is going away
            if type(exc).__module__ != "rtmidi":
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


def _expect_event(port, text, since, timeout):
    """The index after the first event past `since` that contains `text`, or None if none arrives in time."""

    def find():
        return next(
            (i + 1 for i in range(since, len(port.events)) if text in port.events[i]),
            None,
        )

    return port.poll(find, lambda i: i is not None, timeout)


def _expect_oled_changed(port, frame, timeout):
    """Whether the OLED comes to differ from `frame` within `timeout` seconds."""

    def changed(current):
        return current is not None and current != frame

    return frame is not None and changed(port.poll(port.oled, changed, timeout))


def _timeout(args, default=5.0):
    """Splits a trailing timeout in seconds off `args`."""
    try:
        return args[:-1], float(args[-1])
    except (IndexError, ValueError):
        return args, default


def _expect_state(port, state, key, expected, timeout):
    """Polls until state[key] matches `expected`, starting from `state`. Returns (ok, last value)."""

    def match(s):
        return _matches(s.get(key), expected)

    if not match(state):
        state = port.poll(port.state, match, timeout)
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
                for where, text in read_script(included, depth + 1):
                    name = os.path.basename(included)
                    if where.startswith("line "):
                        where = name + ":" + where[len("line ") :]
                    pairs.append((where, text))
            else:
                pairs.append((f"line {number}", raw))
    return pairs


def run_script(port, lines, out=print):
    """Runs a test script against a TestPort. Returns (failed, total) expects.

    `lines` are strings, or (where, text) pairs from read_script(). One command per line; "#" starts a comment.
      include FILE                                 the lines of FILE, relative to the script (read_script() only)
      button ..., pad ..., encoder ..., wait ...   queued on the firmware (see parseLine() in test_port.cpp)
      expect KEY=VALUE [SECONDS]                   polls the state JSON until KEY equals VALUE
      expect oled_changed [SECONDS]                polls until the OLED differs from the last "snapshot"
      expect event TEXT [SECONDS]                  polls until a test event containing TEXT arrives, counting only
                                                   events after the one the previous "expect event" matched
      expect no_event TEXT [SECONDS]               passes if no such event arrives for SECONDS, counted the same way
      midi HEX...                                  sends one MIDI message from the host, e.g. "midi 90 3c 64", once
                                                   the firmware has run every queued event
      snapshot                                     keeps the current OLED frame
      sleep SECONDS                                host-side pause
    Expects time out after 5 s by default. They and snapshots first wait until the firmware has run every queued
    event, waits and encoder turns included.
    """
    failed, total, frame, events_seen = 0, 0, None, len(port.events)
    for number, raw in enumerate(lines, 1):
        where, raw = raw if isinstance(raw, tuple) else (f"line {number}", raw)
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        word, _, rest = line.partition(" ")
        if word in ("button", "pad", "encoder", "wait"):
            port.inject(line)
        elif word == "midi":
            port.wait_idle()
            port.link.send([int(b, 16) for b in rest.split()])
        elif word == "sleep":
            port.pump(float(rest))
        elif word == "snapshot":
            port.wait_idle()
            frame = port.oled()
        elif word == "expect":
            idle = port.wait_idle()
            what, _, rest = rest.partition(" ")
            args, timeout = _timeout(rest.split())
            detail = ""
            if what == "oled_changed" and not args:
                ok = _expect_oled_changed(port, frame, timeout)
            elif what == "event" and args:
                found = _expect_event(port, " ".join(args), events_seen, timeout)
                ok = found is not None
                events_seen = found or events_seen
            elif what == "no_event" and args:
                ok = _expect_event(port, " ".join(args), events_seen, timeout) is None
            elif "=" in what and not args:
                key, _, expected = what.partition("=")
                ok, value = _expect_state(port, idle, key, expected, timeout)
                detail = f"(got {value!r})"
            else:
                raise ValueError(f"{where}: bad expect {line!r}")
            out(
                "{} {}: {} {}".format(
                    "PASS" if ok else "FAIL", where, line, "" if ok else detail
                ).rstrip()
            )
            failed += 0 if ok else 1
            total += 1
        else:
            raise ValueError(f"{where}: unknown command {word!r}")
    return failed, total


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
        choices=("probe", "state", "inject", "events", "run", "ready"),
    )
    ap.add_argument("args", nargs="*", help="inject: event lines; run: the script file")
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
        "--timeout", type=float, default=90, help="ready: how long to wait, in seconds"
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
