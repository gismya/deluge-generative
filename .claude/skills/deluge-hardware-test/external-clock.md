# External clock on the Deluge

For scripts that send MIDI clock, Start, Stop, Continue or song position from the host (`clock BPM`, `midi fa`,
`midi f2 ...`), such as `perform_transport.txt` (its external-clock section) and `perform_clock_matching.txt`.

**Verified on hardware for clock and Start over USB.** `perform_transition_midi.txt` passed on the Deluge on
2026-11-09 (build ea4de0f89): the device follows the host's MIDI clock and Start over USB, with the routing below.
Continue, Stop and song position (the external-clock section of `perform_transport.txt`, and
`perform_clock_matching.txt`) haven't had a hardware run with it yet: for those, rely on delugemu and say so.

## Why a port can ignore the clock

Three settings gate clock and transport input; a message is dropped unless all three let it through:

1. **The cable's clock-in flag** (`MIDICable::receiveClock`, checked first in `MidiEngine::midiMessageReceived`).
   Upstream USB port 1 is built with it on, port 2 and **port 3 with it off** (`upstreamUSBMIDICable1..3` in
   `src/deluge/io/midi/midi_device_manager.cpp`). Sent on Deluge Port 3, the test port, clock and
   transport are dropped. Delugemu passed because its scripts use the DIN port, whose flag is
   on by default.
2. **The global MIDI clock input** (`playbackHandler.midiInClockEnabled`): SETTINGS > MIDI > CLOCK > INPUT. On by
   default, stored in flash.
3. **The echo guard** (`ignoringMidiClockInput`): a Start that arrives within 50 ms of the Deluge sending its own Start
   is taken as an echo and ignored. Irrelevant unless MIDI clock out loops back.

Port 3 can't be switched on from the device: SETTINGS > MIDI > DEVICES lists DIN, upstream USB ports 1 and 2 and hosted
devices, not port 3. Only `SETTINGS/MIDIDevices.XML` on the card (`<upstreamUSBDevice3 receiveClock="1" ...>`) changes
it.

## What the probe does

`RtMidiLink` in `deluge_probe.py` sends real-time messages and song position (`F2`, `F8`, `FA`, `FB`, `FC`) on the first
Deluge port, Port 1, and everything else (sysex, notes, CC) on Port 3. So on the device these must be set:

- SETTINGS > MIDI > CLOCK > INPUT: on (the default).
- SETTINGS > MIDI > DEVICES > UPSTREAM USB PORT 1 > CLOCK IN: on (the default). A card whose `MIDIDevices.XML` turns it
  off overrides the default.

If a clock script still fails on the device, ask the user to check those two settings, then record what they found
here.
