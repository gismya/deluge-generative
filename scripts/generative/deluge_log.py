"""Capture the Deluge's debug log over USB MIDI to the terminal and a file.

Works with a Debug or RelWithDebInfo build (a Release build prints nothing). Finds the Deluge's MIDI ports, sends
the sysex that turns logging on (F0 00 21 7B 01 03 00 01 F7), then prints every log line with the Mac's clock time
and appends it to the log file. Ctrl-C stops it. Run it again after every Deluge reboot: the subscription isn't kept.

Like `dbt sysex-logging`, but decodes the text cleanly (no NUL bytes) and keeps a file, so `grep MEAS` works on it.
Needs python-rtmidi, which the dbt toolchain has, so run it through dbt:

    ./dbt exec "python3 scripts/generative/deluge_log.py --out ~/Desktop/deluge-meas.log"
    ./dbt exec "python3 scripts/generative/deluge_log.py --list"     # show the MIDI ports
"""

import argparse
import datetime
import sys
import time

SUBSCRIBE = [0xF0, 0x00, 0x21, 0x7B, 0x01, 0x03, 0x00, 0x01, 0xF7]
LOG_HEADER = [0xF0, 0x00, 0x21, 0x7B, 0x01, 0x03, 0x40, 0x00]


def pick_port(ports, wanted):
    """The given port, else the last one named Deluge... (the dbt tools use the last: port 3 is for sysex)."""
    if wanted is not None:
        return wanted
    matches = [i for i, name in enumerate(ports) if "deluge" in name.lower()]
    return matches[-1] if matches else None


def main():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument(
        "--out",
        default="deluge-log.txt",
        help="file to append the log to (default: deluge-log.txt)",
    )
    p.add_argument("--list", action="store_true", help="list MIDI ports and exit")
    p.add_argument(
        "--in-port",
        type=int,
        help="MIDI input port number (default: last named Deluge...)",
    )
    p.add_argument(
        "--out-port",
        type=int,
        help="MIDI output port number (default: last named Deluge...)",
    )
    p.add_argument(
        "--only-meas",
        action="store_true",
        help="show only MEAS lines on screen (the file gets all)",
    )
    args = p.parse_args()

    try:
        import rtmidi
    except ImportError:
        sys.exit(
            'python-rtmidi is missing. Run this through dbt: ./dbt exec "python3 '
            + sys.argv[0]
            + '"'
        )

    midi_in = rtmidi.MidiIn()
    midi_out = rtmidi.MidiOut()
    in_ports = midi_in.get_ports()
    out_ports = midi_out.get_ports()
    if args.list:
        print("inputs: ", list(enumerate(in_ports)))
        print("outputs:", list(enumerate(out_ports)))
        return
    in_port = pick_port(in_ports, args.in_port)
    out_port = pick_port(out_ports, args.out_port)
    if in_port is None or out_port is None:
        sys.exit(
            "No Deluge MIDI port. Is it on and connected over USB? Ports: "
            + repr(in_ports)
        )

    midi_in.open_port(in_port)
    midi_in.ignore_types(sysex=False, timing=True, active_sense=True)
    midi_out.open_port(out_port)
    midi_out.send_message(SUBSCRIBE)
    print(
        f"listening on {in_ports[in_port]!r}, logging to {args.out}; Ctrl-C stops",
        flush=True,
    )

    with open(args.out, "a", encoding="ascii", errors="replace") as fh:
        fh.write(
            f"# deluge_log.py started {datetime.datetime.now().astimezone().isoformat(timespec='seconds')}\n"
        )
        partial = ""
        try:
            while True:
                msg = midi_in.get_message()
                if not msg:
                    time.sleep(0.005)
                    continue
                data = msg[0]
                if data[: len(LOG_HEADER)] != LOG_HEADER:
                    continue
                partial += bytes(b & 0x7F for b in data[len(LOG_HEADER) : -1]).decode(
                    "ascii", "replace"
                )
                while "\n" in partial:
                    line, partial = partial.split("\n", 1)
                    stamp = (
                        datetime.datetime.now()
                        .astimezone()
                        .strftime("%H:%M:%S.%f")[:-3]
                    )
                    fh.write(f"{stamp} {line}\n")
                    fh.flush()
                    if not args.only_meas or "MEAS " in line:
                        print(f"{stamp} {line}", flush=True)
        except KeyboardInterrupt:
            pass
        finally:
            midi_out.send_message(
                [0xF0, 0x00, 0x21, 0x7B, 0x01, 0x03, 0x00, 0x00, 0xF7]
            )


if __name__ == "__main__":
    main()
