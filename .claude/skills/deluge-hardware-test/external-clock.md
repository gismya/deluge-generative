# External clock on the Deluge

For scripts that send MIDI clock, Start, Stop, Continue or song position from the host (`clock BPM`, `midi fa`,
`midi f2 ...`), such as `perform_transport.txt` (its external-clock section) and `perform_clock_matching.txt`.

**Unverified on hardware.** Worked out from the firmware on 2026-11-07, after hardware run 1, where those parts failed
on the device (it never started on the host's Start and clock) while delugemu passed. Nothing has been run on the
device since the change below. Until a hardware run passes, rely on delugemu for these parts and say so.

## Why the device ignored the clock

Three settings gate clock and transport input; a message is dropped unless all three let it through:

1. **The cable's clock-in flag** (`MIDICable::receiveClock`, checked first in `MidiEngine::midiMessageReceived`).
   Upstream USB port 1 is built with it on, port 2 and **port 3 with it off** (`upstreamUSBMIDICable1..3` in
   `src/deluge/io/midi/midi_device_manager.cpp`). The probe used to send everything on Deluge Port 3, the test port,
   so the device dropped clock and transport there. Delugemu passed because its scripts use the DIN port, whose flag is
   on by default.
2. **The global MIDI clock input** (`playbackHandler.midiInClockEnabled`): SETTINGS > MIDI > CLOCK > INPUT. On by
   default, stored in flash.
3. **The echo guard** (`ignoringMidiClockInput`): a Start that arrives within 50 ms of the Deluge sending its own Start
   is taken as an echo and ignored. Irrelevant unless MIDI clock out loops back.

Port 3 can't be switched on from the device: SETTINGS > MIDI > DEVICES lists DIN, upstream USB ports 1 and 2 and hosted
devices, not port 3. Only `SETTINGS/MIDIDevices.XML` on the card (`<upstreamUSBDevice3 receiveClock="1" ...>`) changes
it.

## What the probe does now

`RtMidiLink` in `deluge_probe.py` sends real-time messages and song position (`F2`, `F8`, `FA`, `FB`, `FC`) on the first
Deluge port, Port 1, and everything else (sysex, notes, CC) on Port 3 as before. So on the device these must be set:

- SETTINGS > MIDI > CLOCK > INPUT: on (the default).
- SETTINGS > MIDI > DEVICES > UPSTREAM USB PORT 1 > CLOCK IN: on (the default). A card whose `MIDIDevices.XML` turns it
  off overrides the default.

If a clock script still fails on the device, ask the user to check those two settings, then record what they found
here.
