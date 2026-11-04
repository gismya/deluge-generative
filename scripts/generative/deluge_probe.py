"""Read-only Deluge USB-MIDI probe.

Lists CoreMIDI ports. If a port named like "Deluge" exists, sends only read-only requests:
universal identity request, Deluge ping, one OLED dump (RLE), and optionally a debug-log
subscription for N seconds (then unsubscribes). Sends nothing to non-Deluge ports.

Run with the dbt toolchain python (has python-rtmidi 1.5.8):
    ./dbt exec "python3 scripts/generative/deluge_probe.py [--log-seconds N] [--selftest]"
See docs/hardware-measurements/deluge-usb-control.md.
"""

import argparse
import random
import sys
import time

IDENTITY_REQ = [0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7]
PING = [0xF0, 0x00, 0x21, 0x7B, 0x01, 0x00, 0xF7]
OLED_RLE = [0xF0, 0x00, 0x21, 0x7B, 0x01, 0x02, 0x00, 0x01, 0xF7]
LOG_ON = [0xF0, 0x00, 0x21, 0x7B, 0x01, 0x03, 0x00, 0x01, 0xF7]
LOG_OFF = [0xF0, 0x00, 0x21, 0x7B, 0x01, 0x03, 0x00, 0x00, 0xF7]
HDR = [0xF0, 0x00, 0x21, 0x7B, 0x01]


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


def collect(midiin, seconds):
    out, end = [], time.time() + seconds
    while time.time() < end:
        m = midiin.get_message()
        if m:
            out.append(m[0])
        else:
            time.sleep(0.005)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log-seconds", type=float, default=0)
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()

    if a.selftest:
        rnd = random.Random(1)
        for _ in range(200):
            data = bytes(
                rnd.choice([0, 0, 0, 255, rnd.randrange(256)]) for _ in range(768)
            )
            assert unpack_7to8_rle(pack_8to7_rle(data)) == data
        print("selftest: RLE round-trip ok (200 x 768 bytes)")

    import rtmidi

    mi, mo = rtmidi.MidiIn(), rtmidi.MidiOut()
    ins, outs = mi.get_ports(), mo.get_ports()
    print("MIDI in :", ins)
    print("MIDI out:", outs)
    di = [i for i, p in enumerate(ins) if "deluge" in p.lower()]
    do = [i for i, p in enumerate(outs) if "deluge" in p.lower()]
    if not di or not do:
        print("No Deluge MIDI port. Nothing sent.")
        return 0

    mi.ignore_types(sysex=False, timing=True, active_sense=True)
    mi.open_port(di[-1])
    mo.open_port(do[-1])
    print("using", ins[di[-1]], "/", outs[do[-1]])

    mo.send_message(IDENTITY_REQ)
    mo.send_message(PING)
    for m in collect(mi, 1.0):
        if m[:5] == [0xF0, 0x7E, 0x7F, 0x06, 0x02]:
            print("identity reply, firmware {}.{}.{}".format(*m[12:15]), m)
        elif m[:6] == HDR + [0x7F]:
            print("pong", m)

    mo.send_message(OLED_RLE)
    for m in collect(mi, 1.0):
        if m[:8] == HDR + [0x02, 0x40, 0x01]:
            img = unpack_7to8_rle(m[9:-1])
            print(f"OLED ({len(img)} bytes):")
            print(oled_ascii(img))

    if a.log_seconds > 0:
        mo.send_message(LOG_ON)
        try:
            for m in collect(mi, a.log_seconds):
                if m[:8] == HDR + [0x03, 0x40, 0x00]:
                    sys.stdout.write(bytes(m[8:-1]).decode("ascii", "replace"))
        finally:
            mo.send_message(LOG_OFF)
    return 0


if __name__ == "__main__":
    sys.exit(main())
