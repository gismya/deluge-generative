"""SD card access on a Deluge over USB MIDI, through the firmware's JSON file API (src/deluge/storage/smsysex.cpp).

    ./dbt exec "python3 scripts/generative/deluge_files.py dir SAMPLES/DRUMS"
    ./dbt exec "python3 scripts/generative/deluge_files.py delete /SONGS/GENTEST1.XML"

Requests are F0 00 21 7B 01 04 <seq> {json} F7, replies F0 00 21 7B 01 05 <seq> {json} [00 <packed data>] F7.
"""

import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import deluge_probe as dp

JSON_REQ, JSON_REPLY = 0x04, 0x05


class Files:
    def __init__(self, link=None):
        self.link = link or dp.RtMidiLink(quiet=True)
        self.seq = 0

    def request(self, obj, data=None, timeout=10.0):
        self.seq = self.seq % 127 + 1
        msg = (
            dp.HDR
            + [JSON_REQ, self.seq]
            + list(json.dumps(obj, separators=(",", ":")).encode("ascii"))
        )
        if data is not None:
            msg += [0] + dp.pack_8to7(list(data))
        self.link.send(msg + [0xF7])
        end = time.time() + timeout
        while time.time() < end:
            m = self.link.receive(end - time.time())
            if m is None:
                break
            if m[:6] == dp.HDR + [JSON_REPLY] and m[6] == self.seq:
                body = bytes(m[7:-1])
                text, _, packed = body.partition(b"\x00")
                return json.loads(text.decode("ascii")), packed
        raise TimeoutError(f"no reply to {obj}")

    def dir(self, path, lines=50):
        out, offset = [], 0
        while True:
            reply, _ = self.request(
                {"dir": {"path": path, "offset": offset, "lines": lines}}
            )
            entries = reply["^dir"]["list"]
            out += entries
            if len(entries) < lines:
                return out
            offset += len(entries)

    def delete(self, path):
        reply, _ = self.request({"delete": {"path": path}})
        return reply

    def write_file(self, path, data, block=512):
        reply, _ = self.request({"open": {"path": path, "write": 1}})
        fid = reply["^open"]["fid"]
        try:
            for addr in range(0, len(data), block):
                chunk = data[addr : addr + block]
                self.request(
                    {"write": {"fid": fid, "addr": addr, "size": len(chunk)}}, chunk
                )
        finally:
            self.request({"close": {"fid": fid}})

    def read_file(self, path, block=512):
        reply, _ = self.request({"open": {"path": path, "write": 0}})
        fid = reply["^open"]["fid"]
        size = reply["^open"].get("size", 0)
        data = bytearray()
        try:
            while len(data) < size:
                r, packed = self.request(
                    {"read": {"fid": fid, "addr": len(data), "size": block}}
                )
                got = bytes(dp_unpack(packed))
                if not got:
                    break
                data += got[: r["^read"].get("size", len(got))]
        finally:
            self.request({"close": {"fid": fid}})
        return bytes(data)


def dp_unpack(packed):
    """Inverse of pack_8to7: groups of a high-bit byte plus up to 7 bytes."""
    out = []
    for i in range(0, len(packed), 8):
        hb, chunk = packed[i], packed[i + 1 : i + 8]
        out += [b | (0x80 if hb & (1 << j) else 0) for j, b in enumerate(chunk)]
    return out


def main():
    cmd, *args = sys.argv[1:]
    f = Files()
    if cmd == "dir":
        for e in f.dir(args[0]):
            kind = "d" if e["attr"] & 0x10 else "-"
            print(f"{kind} {e['size']:>10} {e['name']}")
    elif cmd == "delete":
        print(f.delete(args[0]))
    else:
        raise SystemExit(f"unknown command {cmd}")


if __name__ == "__main__":
    main()
