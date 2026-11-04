# Driving and observing a real Deluge from this Mac

Can an agent on this Mac control and observe a physical Deluge over USB, and what is missing for an unattended hardware test? Researched 2026-11-04 against this repo (`generative`, based on `v1.3.0-rc3`) and upstream `main` at `e0497fe3f` (2026-11-03).

Short answer: over USB MIDI sysex an agent can **observe** the device (OLED mirror, debug log, firmware version, SD card reads) and can **load firmware into RAM**. It **cannot press buttons, pads or encoders**. Neither this repo nor upstream `main` has any input-injection message. Plain MIDI notes can drive a few transport functions, but only after a human has learned them on the device.

## 1. What the firmware exposes over USB MIDI

### Transport

- USB is MIDI only. There is one interface (Audio class, MIDI subclass) with 3 cables. There is no USB audio, so audio tests need an analog capture. Sources: `src/deluge/io/midi/r_usb_pmidi_descriptor.c:105`, `:112`, `:126-127`, `:217`, `:235`.
- The USB VID:PID is `0x16D0:0x0CE2` (`r_usb_pmidi_descriptor.c:41-42`). CoreMIDI port names contain "Deluge", for example "Deluge Port 1" ([Vuefinder doc](https://delugecommunity.com/resources/applications/vuefinder/), in-repo `website/src/content/docs/resources/applications/Vuefinder.md:7`).
- Replies go back on the cable the request came in on. The dbt tools use the **last** Deluge port ("port 3 is reserved for sysex"): `scripts/tasks/util.py:256-264`.
- Incoming sysex is reassembled into a 1024-byte buffer per cable (`src/deluge/io/midi/midi_device.h:161`). Longer messages are silently dropped (`src/deluge/io/midi/midi_engine.cpp:783-787`).
- Dispatch happens in `MidiEngine::midiSysexReceived` (`midi_engine.cpp:801-876`). It runs in the scheduler's "midi routine" task, not an ISR: `src/deluge/deluge.cpp:560` → `src/deluge/playback/playback_handler.cpp:125-131` → `midi_engine.cpp:793`.

### Message map (header `F0 00 21 7B 01 <cmd> …`, `src/deluge/io/midi/sysex.h:38-58`)

| Request | Bytes | Builds | Effect / reply | Source |
|---|---|---|---|---|
| Universal identity | `F0 7E 7F 06 01 F7` | all | `F0 7E 7F 06 02 00 21 7B 01 00 00 00 MAJ MIN PATCH 00 F7` | `midi_engine.cpp:807-825` |
| Ping | `F0 00 21 7B 01 00 F7` | all | Pong `F0 00 21 7B 01 7F 00 F7` | `midi_engine.cpp:845-851` |
| Popup | `… 01 F7` | all | Shows a fixed "hello sysex" popup. Writes to the screen. | `midi_engine.cpp:854-856` |
| OLED dump | `… 02 00 00 F7` (raw) / `… 02 00 01 F7` (RLE) | all | `… 02 40 <0\|1> 00 <packed 768 bytes> F7` | `src/deluge/hid/hid_sysex.cpp:43-48`, `:100-133` |
| OLED auto-push | `… 02 00 02 F7` (delta) / `… 02 00 03 F7` (forced full) | all | Pushes `… 02 40 02 <start> <len> <RLE> F7` on every changed frame for **2 s** after each request | `hid_sysex.cpp:50-70`, `:78-98`, `:158-211`; hook `src/deluge/hid/display/oled.cpp:443` |
| Swap display type | `… 02 00 04 F7` | all | Toggles OLED/7SEG emulation. Changes device state. | `hid_sysex.cpp:72-75` |
| 7SEG dump | `… 02 01 00 F7` | all | No-op on OLED hardware | `hid_sysex.cpp:135-156` |
| Debug log subscribe | `… 03 00 01 F7` / unsubscribe `… 03 00 00 F7` | all accept it; only Debug/RelWithDebInfo print | Log lines as `… 03 40 00 <7-bit ASCII> F7` | `src/deluge/io/midi/sysex.cpp:36-44`, `:63-92` |
| Firmware segment | `… 03 01 <key:5> <idx lo> <idx hi> <586 packed> F7` | `ENABLE_SYSEX_LOAD` only | Loads 512 bytes into a RAM buffer | `sysex.cpp:46-50`, `:139-180` |
| Firmware run | `… 03 02 <key, len, crc32: 14 packed> F7` | `ENABLE_SYSEX_LOAD` only | CRC check, then `chainload_from_buf` | `sysex.cpp:52-56`, `:182-213` |
| JSON file API | `… 04 <seq> {json} [00 <packed>] F7` | all (since c1.3) | Reply `… 05 <seq> {json} …` | `midi_engine.cpp:868-870`, `src/deluge/storage/smsysex.cpp:767-850` |

`0x7D` (the "non-commercial" ID) is also accepted in place of `00 21 7B 01` (`midi_engine.cpp:827-835`).

### Debug log

- `D_PRINTLN`/`D_PRINT` compile to nothing unless `ENABLE_TEXT_OUTPUT` is set (`src/deluge/io/debug/log.h:38-48`). It is set only for the **Debug** and **RelWithDebInfo** configs (`src/deluge/CMakeLists.txt:45-48`). Release (`./dbt build release`) logs nothing. RelWithDebInfo is `-O2` with LTO like Release (`CMakeLists.txt:169-187`), so it is the right config for timing-sensitive hardware tests.
- Each line is prefixed with `"<seconds>: <file>:<line>: "` (`src/deluge/io/debug/log.cpp:43-44`).
- Routing (`src/deluge/io/debug/print.cpp:124-154`): once a cable has subscribed, output goes there as sysex. Otherwise it goes to SEGGER RTT (`src/deluge/drivers/uart/uart.c:36-50`, `HAVE_RTT` set for Debug/RelWithDebInfo at `src/deluge/CMakeLists.txt:50-58`). The subscription is a single pointer held in RAM, so it is lost on reboot or chainload and has to be re-sent.
- The category byte (`reply_hdr[7]`) is always `00`. The code comment calls it "reserved, could serve as a message identifier" (`sysex.cpp:67-70`). It is a free slot for a test-event channel.
- `ENABLE_MATRIX_DEBUG` (CMake option, `CMakeLists.txt:76-77`) logs every button/pad with the current UI name (`src/deluge/hid/buttons.cpp:85-87`, `src/deluge/hid/matrix/matrix_driver.cpp:76-78`, names at `src/deluge/gui/ui/ui.cpp:476-484`, which include `generative_perform`).

### OLED mirroring

- The frame is 128×48, 1 bpp, 768 bytes: 6 pages × 128 columns, bit *n* = row 8·page+*n* (`src/RZA1/cpu_specific.h:137`, `:145`; decoder in [DelugeWeb app.js](https://github.com/SynthstromAudible/DelugeWeb/blob/main/delugeclient/app.js) `drawOleddata`).
- Mode 0 uses 8→7 packing (`src/deluge/util/pack.c:7-29`). Modes 1/2 use the custom 7-bit RLE (`pack.c:94-150`, decoder `pack.c:152-210`).
- Auto-push stops 2 s after the last request (`hid_sysex.cpp:55`), so a host must re-request about once a second. DelugeWeb does this with `setInterval(getDisplay, 1000)`.
- This works in Release builds. It is the only way to read popups and Perform view state without a debug build.

### Firmware upload over sysex ("loadfw")

- The build needs `./dbt configure -DENABLE_SYSEX_LOAD=YES` (`CMakeLists.txt:73-74`, `src/deluge/CMakeLists.txt:60-63`). That build must first be flashed from SD by a human (`website/src/content/docs/development/deluge/tools.md:16-35`).
- A human must also enable SETTINGS > COMMUNITY FEATURES > "Allow insecure develop sysex messages". That generates a random 31-bit key and shows it on screen (`src/deluge/gui/menu_item/runtime_feature/devSysexSetting.cpp:30-41`). The key is persisted as `devSysexAllowed` in `SETTINGS/CommunityFeatures.XML` (`src/deluge/model/settings/runtime_feature_settings.cpp:28`, `:128`, `:330-335`), so an agent could read it back with the JSON file API.
- Host side: `./dbt loadfw relwithdebinfo` (`scripts/tasks/task-loadfw.py`). Packet format is at `:80-127`. The key is cached in `.deluge_hex_key` (`:160-172`), and there is a 2 ms delay between packets (`:51-55`).
- The firmware is loaded **into RAM only**. A power cycle boots whatever was last flashed from SD (`tools.md:39-50`). That is good for unattended tests because a power cycle is a reliable rollback.
- **A wrong key freezes the device** (`FREEZE_WITH_ERROR("E997")`, `sysex.cpp:153-155`). So do a missing buffer (`E999`), a disabled setting (`E996`) and a missing run packet (`E995`) (`sysex.cpp:164-166`, `:184-192`). Recovery needs a human to power-cycle.

### SD card access (JSON protocol)

- It is present in v1.3.0-rc3 (`git ls-tree v1.3.0-rc3 src/deluge/storage/smsysex.cpp`).
- Verbs: `session`, `open`, `close`, `dir`, `read`, `write`, `delete`, `mkdir`, `rename`, `copy`, `move`, `utime`, `ping` (`smsysex.cpp:792-845`). Spec: `website/src/content/docs/development/deluge/SysExProtocolNotes.md` = <https://delugecommunity.com/development/deluge/sysexprotocolnotes/>. Limits: 4 open files, 1024 B per block, 25 directory entries per request.
- Requests are queued and handled by a scheduler task that needs `RESOURCE_SD` (`smsysex.cpp:767-785`, `deluge.cpp:586`), so they wait while the firmware is using the card. Throughput is about 200 KB/s (`Vuefinder.md:21`).
- `HID 02 02` dumps the JSON deserializer's cluster buffer (`hid_sysex.cpp:213-238`). It is a leftover debug aid and of no use here.

### Remote input

- **None.** Every path into `Buttons::buttonAction`, `MatrixDriver::padAction` and the encoder accumulators starts at the PIC UART (`deluge.cpp:271-306`) or the encoder IRQ/poll (`src/deluge/hid/encoders.cpp:82-165`). There are only three sysex HID subcommands, and all are display reads (`hid_sysex.cpp:25-40`). Upstream `main` has the same `SysexCommands` enum and no new HID subcommands (`git diff HEAD upstream/main` touches only USB suspend and a `writeBlock` null check).
- Partial substitute: **global MIDI commands** (PLAY, RESTART, RECORD, TAP, LOOP, UNDO, REDO, FILL, NEXT_SONG, SHIFT toggle; `src/definitions_cxx.hpp:585-600`, handled at `src/deluge/playback/playback_handler.cpp:2889-2955`). They act when a learned note/CC arrives, and a human must learn each one in the MIDI settings menu first. PLAY calls `playButtonPressed` directly. SHIFT toggles sticky shift. Nothing here enters Perform view, turns gold knobs or presses SAVE.
- Incoming MIDI clock can drive tempo. Outgoing clock timing can be measured with `./dbt midi-clock` (`scripts/tasks/task-midi-clock.py:13`).

## 2. Host tools

| Tool | What | Headless from Mac CLI? | Source |
|---|---|---|---|
| `./dbt sysex-logging [port]` | Subscribes to the log and prints it | Yes. Uses the dbt toolchain Python with python-rtmidi 1.5.8 | `scripts/tasks/task-sysex-logging.py`, `tools.md:53-65` |
| `scripts/generative/deluge_log.py` (this branch) | Subscribes, prints clean text with Mac timestamps, appends to a file | Yes, via `./dbt exec "python3 scripts/generative/deluge_log.py --out FILE"` | written for #37; decoder checked against delugemu, not yet against hardware |
| `scripts/generative/deluge_probe.py` (this branch) | Read-only probe: identity, ping, one OLED dump as ASCII art, optional timed log | Yes, via `./dbt exec "python3 scripts/generative/deluge_probe.py"` | RLE decoder self-tested; not yet run against hardware |
| `./dbt loadfw <build>` | RAM firmware upload | Yes, once `.deluge_hex_key` exists | `scripts/tasks/task-loadfw.py` |
| `./dbt midi-clock` | Measures the Deluge's MIDI clock-out intervals | Yes | `scripts/tasks/task-midi-clock.py` |
| DEx (`dex.silicak.es`) | OLED/7SEG mirror, debug console ("Auto Debug"), file browser, raw sysex | No (WebMIDI page, Chrome) | [silicakes/deluge-extensions](https://github.com/silicakes/deluge-extensions) |
| DelugeWeb `delugeclient` | Ping, OLED/7SEG fetch, 1 s refresh, debug log | No (web, "not updated recently") | [DelugeWeb README](https://github.com/SynthstromAudible/DelugeWeb) |
| Vuefinder | SD file browser over the JSON protocol | No (web) | `Vuefinder.md`, [source](https://github.com/SynthstromAudible/DelugeWeb/tree/main/vuefinder) |
| DelugeDisplay | Native macOS/iOS OLED mirror | GUI app | [douglas-carmichael/DelugeDisplay](https://github.com/douglas-carmichael/DelugeDisplay/releases), `overview.mdx:48-57` |

Quirks found while checking:

- `./dbt` refuses to run in a git **worktree** because `.git` is a file there (`dbt:28-33`). Use `DBT_NO_SYNC=1 ./dbt …`. This was verified here.
- `task-sysex-logging.py`'s `unpack_7bit_to_8bit` (`:32-49`) interleaves a NUL after every byte and keeps the `03 40 00` header. On a terminal this shows as a stray `@` with readable text, but `grep` treats the piped output as binary. Pipe through `tr -d '\000'`, or decode `msg[8:-1]` as ASCII yourself. The payload is plain 7-bit ASCII (`sysex.cpp:79-82`). Simulated locally:
  `'\x03\x00@\x00\x00\x001\x00.\x002\x00…'` for an input line `1.2345: foo.cpp:12: hi`.
- **Bursts of log lines get dropped.** Both sysex senders write into fixed output buffers and drop what doesn't fit: USB `MIDICableUSB::sendSysex` → `bufferMessage` (`src/deluge/io/midi/cable_types/usb_common.cpp:80-130`), DIN `bufferMIDIUart` (`cable_types/din.cpp:52-62`, "beware of MIDI_TX_BUFFER_SIZE"). In delugemu over DIN, a 30-line report arrived with lines missing. The #37 measurement code works around it by flushing and waiting for `midiEngine.anythingInOutputBuffer()` to clear after each line (`src/deluge/generative/debug/hardware_measurements.cpp`, `logLine`). Any agent-facing log channel should do the same, or rate-limit.
- **delugemu's USB-MIDI model is one-way here.** With `--usb-midi udp:…`, the firmware's output reaches the host, but host bytes are framed and queued and never delivered (`rza1l-usb: after framing: rx_q_count=… recv_armed=0x0` with `RZA1L_USB_DEBUG=1`). So emulator tests that need to send sysex use the DIN port (`--midi udp:…`); the firmware accepts the log subscription on any cable. `scripts/generative/measurements_emu.py` does this.
- `task-gdb.py:32-38` still builds SCons-era targets (`dbt-build-debug-oled`), so it is stale. `IDE_Configs/vscode/launch.json:212-223` references `contrib/renesas-rz-a1lu.cfg`, which does not exist, and a Windows OpenOCD path.

Installed on this Mac (2026-11-04):

| Item | Status |
|---|---|
| `/opt/homebrew/bin/python3` 3.15.2 | no `rtmidi`, no `mido` |
| dbt toolchain Python 3.12.8 (`toolchain/current/python/bin/python3`) | **python-rtmidi 1.5.8, CoreMidi API**, pinned in `scripts/toolchain/requirements.txt` |
| Node v26.13.0, global modules | `chrome-devtools-mcp`, `npm`, `typescript`, `typescript-language-server`; no MIDI library |
| `sendmidi` / `receivemidi` | not installed |
| `swift` | `/usr/bin/swift` (CoreMIDI possible, not needed) |
| OpenOCD | toolchain xPack 0.12.0+dev (`toolchain/current/openocd/bin/openocd`) |
| `arm-none-eabi-gdb` | in toolchain (`toolchain/current/arm-none-eabi-gcc/bin/`), not on PATH |
| J-Link tools, pyOCD, probe-rs | not installed |

## 3. Debug probe (SWD/JTAG)

- **Header:** a 2×5 pin header next to the Renesas RZ/A1L. It is unpopulated, so you have to solder one on (Amphenol FCI 20021111-00010), and that means taking the PCB out of the case. Fitting it "will probably void your hardware warranty". Sources: [Uploading, debugging … wiki](https://github.com/SynthstromAudible/DelugeFirmware/wiki/Uploading,-debugging,-and-RAM,-flash-operation-explanation), [Hardware modification guide](https://github.com/SynthstromAudible/DelugeFirmware/wiki/Hardware-modification-guide). Neither page gives a pin-by-pin pinout.
- **Probes:** a J-Link (EDU Mini known to work, needs an adapter) or a DelugeProbe, which is CMSIS-DAP on a Pi Pico, about $3–20, with RTT channel 0 over USB CDC ([wiki](https://github.com/SynthstromAudible/DelugeFirmware/wiki/Uploading,-debugging,-and-RAM,-flash-operation-explanation); [litui/delugeprobe](https://github.com/litui/delugeprobe), `docs/CONTRIBUTING.md:36`).
- **Repo support:**
  - OpenOCD configs: `scripts/debug/openocd/deluge-swj-swd.cfg` (`r7s721020`, `cortex_a`, semihosting enabled at `:18-22`, `:57`), `deluge-swj-jtag.cfg`, `interface/delugeprobe.cfg` (CMSIS-DAP).
  - dbt tasks: `./dbt openocd` (`scripts/tasks/task-openocd.py:44-48`) and `./dbt debug [-j]` for a J-Link or OpenOCD GDB server on :3333 (`scripts/tasks/task-debug.py:26-65`).
  - Ozone projects: `contrib/debug/*.jdebug`. SVD: `contrib/rza1.svd`. VS Code: `IDE_Configs/vscode/launch.json`, with J-Link and RTT console at `:5-42`.
- **RTT:** compiled into Debug/RelWithDebInfo only. One 8 KB up-buffer (`src/RTT/SEGGER_RTT_Conf.h:72`, `:80`). The wiki says RTT "does not seem to work while OpenOCD is connected".
- **Crashes:** a CPU fault paints PC/LR/stack pointers and the commit ID onto the **pad LEDs**, then spins forever (`src/OSLikeStuff/fault_handler/fault_handler.c:230-305`). That is invisible over USB. Only RTT (`:273-275`) or a camera/human sees it. After a fault, USB MIDI stops answering pings, and that is the only crash signal an agent gets.
- Probes can load into RAM or flash (DelugeProbe/OpenOCD). This route works, but it needs hardware modification, so it is not the default plan here.

## Probe results on this Mac

Date: 2026-11-04, about 20:42–20:47 CET.

| Command | Result |
|---|---|
| `system_profiler SPUSBDataType` | Empty output. On this macOS (Darwin 27) the data type is `SPUSBHostDataType`. |
| `system_profiler SPUSBHostDataType` | VIA Labs hubs, a Genesys "USB Storage" (05e3:0751), a USB-C video adaptor. A Hügelton Instruments device re-enumerated during the session as "FM-1" (0x4c4a:0xc755) and then "Felucca Update" (0x1209:0x0002). **No Deluge (0x16D0:0x0CE2).** |
| `toolchain/current/python/bin/python3 -I -c 'import rtmidi; …get_ports()'` | python-rtmidi 1.5.8, API CoreMidi. Ports over time: `[]`, then `['FM-1']`, then `['Felucca Update']`. No port named Deluge. |
| `python3 -c 'import rtmidi'` / `import mido` (Homebrew 3.15.2) | `ModuleNotFoundError` for both |
| `npm root -g` listing; `command -v sendmidi receivemidi openocd arm-none-eabi-gdb JLinkGDBServerCL pyocd probe-rs swift` | see the install table above |
| `./dbt sysex-logging` | `".git" directory not found` (worktree) |
| `DBT_NO_SYNC=1 ./dbt sysex-logging` | `Could not identify output port for Deluge. Aborting.` Available ports: none. |
| `python3 -I scratchpad/probe/deluge_probe.py --selftest` (toolchain Python) | `selftest: RLE round-trip ok (200 x 768 bytes)`, port list `['Felucca Update']`, `No Deluge MIDI port. Nothing sent.` |

No sysex was sent to any device. The probe script sends only to ports whose name contains "deluge". It sends identity request, ping, one RLE OLED dump, and with `--log-seconds N` a log subscribe followed by an unsubscribe. It decodes the reply into ASCII art. It is committed as `scripts/generative/deluge_probe.py`.

**What I would run once a Deluge is connected** (read-only, no go-ahead needed beyond plugging it in): `./dbt exec "python3 scripts/generative/deluge_probe.py --log-seconds 10"` to confirm identity, ping, OLED dump and (with a Debug build) the log. **What needs the user's go-ahead first** and was not done: uploading firmware (`loadfw`), any JSON `write`/`delete`/`rename` on the card, and any input event (none exist anyway).

## What an agent can do today

| Capability | Status | How |
|---|---|---|
| Identify firmware | yes | identity request |
| Liveness / freeze detection | yes | ping every few seconds; a fault or freeze stops replies |
| See the screen | yes, any build | OLED auto-push, re-requested every 1 s |
| Read logs | Debug/RelWithDebInfo only | `03 00 01` subscribe, then parse `03 40 00` |
| Read/write SD card | yes | JSON API (`dir`, `open`, `read`, …) |
| Update firmware | RAM only, after one-time human setup | `ENABLE_SYSEX_LOAD` build flashed from SD, key enabled, `DBT_NO_SYNC=1 ./dbt loadfw relwithdebinfo` |
| Start/stop, tap tempo, fill | only after a human learns global MIDI commands | MIDI note to a learned command |
| Change tempo | yes, if the Deluge follows external clock | send MIDI clock |
| Press buttons/pads, turn encoders | **no** | none |
| Hear audio | **no** over USB | line out → audio interface |
| See crash pointers | **no** | pad LEDs only; RTT with a probe |

## Recommendation

**Smallest firmware addition: a sysex test port behind a compile flag.** Add `ENABLE_SYSEX_TEST` (CMake option, default OFF, modelled on `ENABLE_SYSEX_LOAD` at `CMakeLists.txt:73-74` and `src/deluge/CMakeLists.txt:60-63`) with three messages under the existing HID (`02`) and Debug (`03`) namespaces.

1. **Inject** `F0 00 21 7B 01 02 10 <seq> <ascii line> F7`. Reply: `… 02 50 <seq> <status> F7`, where status is queued, queue full or parse error. The line syntax is copied from delugemu's `dz-event` (`~/source/delugemu/pylib/delugemu/session.py:245-256`), so one test script can drive both the emulator and hardware:
   - `button <x> <y> down|up`, or a name from `KnownButtons` (`src/deluge/hid/button.h:24-51`), e.g. `button SHIFT down`
   - `pad <x> <y> down|up [vel]`
   - `encoder <i> <detents>`, with i: 0 scrollY, 1 scrollX, 2 tempo, 3 select, 4 upper gold, 5 lower gold, as in `scripts/generative/perform_poc_emu.py:58-61`
   - `wait <ms>`, so a chord such as SHIFT+SONG keeps firmware-side timing instead of USB jitter
2. **State query** `… 02 11 <seq> F7`, replying with `… 02 51 <seq> {json} F7`. The JSON carries:
   - UI name: un-`#if` `getUIName`, `src/deluge/gui/ui/ui.h:175-177`, `ui.cpp:476-484`
   - `currentUIMode`, playing, BPM, swing
   - `AudioEngine::getNumVoices()` and `cpuDireness` (`src/deluge/processing/engines/audio_engine.h:201`, `:218`)
   - `sdRoutineLock`, `currentlyAccessingCard`, inject queue depth, `audioSampleTimer`
   - Generative counters: voice-start overflow/steal count, last Regenerate, hat-choke count, last load duration
   This makes assertions possible without parsing log text.
3. **Event channel.** Reuse the debug log frame (`sysex.cpp:63-92`) with category byte `01` instead of `00`, and add a `TEST_EVENT(fmt, …)` macro that is live whenever `ENABLE_SYSEX_TEST` is set, **including Release**. Events such as `voice_overflow`, `perform_enter`, `save_done ms=…` and `load_done ms=…` then reach the host without a Debug build.

**Where injection has to run.** The sysex handler runs in the "midi routine" task, which checks only `RESOURCE_SD|RESOURCE_USB` (`deluge.cpp:560`, `src/OSLikeStuff/task_scheduler/resource_checker.h:33-48`). That means it can run while a song load is yielding inside the SD routine. Calling `buttonAction` from there would re-enter the UI mid-load. So the handler should only parse into a fixed ring buffer (about 64 entries, no allocation), and the queue is drained elsewhere:

- **Buttons and pads:** in `readButtonsAndPads()` (`deluge.cpp:249`, task at `:577`). Drain only when no PIC byte is pending, one event per call. Feed the same branch the hardware uses: factor `deluge.cpp:275-306` into `handleHidEvent(value, on)`, building pad bytes with `Pad(x,y).toChar()` (`src/deluge/hid/matrix/pad.cpp:16-23`). Injected events then get the existing `sdRoutineLock` / `REMIND_ME_OUTSIDE_CARD_ROUTINE` retry for free. The retry needs a "peek/pop" on the ring rather than `uartPutCharBack`.
- **Encoders:** add detents through `DetentedEncoder::restore(n)` (`src/deluge/hid/encoders.h:64`, an atomic `fetch_add`) and gold edges through `ContinuousEncoder::apply_edges` (`encoders.h:83`). `interpretEncoders` (`src/deluge/hid/encoder_input.cpp:52-190`) then applies its SD-routine deferral as usual. Gold knobs go through acceleration and the first-wiggle filter (`encoder_input.cpp:170-188`). For exact values, feed ±1 edge per encoder-task tick (1 ms, `deluge.cpp:556`). Do not batch them.

**Safety.** Keep it compile-time only, off in the shipped generative build. Optionally also require the `devSysexAllowed` key in the message as `loadfw` does, but on a mismatch reply with an error instead of `FREEZE_WITH_ERROR`.

**Size.** About 200–300 lines of firmware: about 120 for ring + parser + reply, 30 for the `readButtonsAndPads` refactor, 60–80 for the state JSON, 10 for the event macro and CMake. That is roughly 2–4 KB of code. The host side is about 250 lines of Python: extend the probe script and add the inject/state/event client on the toolchain's python-rtmidi. It is new code, nothing ported from upstream.

**How it covers the hardware-test ticket.**

| Ticket item | Drive | Observe |
|---|---|---|
| Voice-start overflow at high Density | `encoder 5 +N` (lower gold = Density) | overflow counter in state, `TEST_EVENT` |
| Swing | encoder/menu injection | state `swing`, MIDI clock-out timing via `dbt midi-clock` |
| Tempo changes | `encoder 2 ±N` or external clock | state BPM, clock-out intervals |
| Long-run stability | loop of the above | ping + state every 5 s. A missing pong means a crash; a human reads the pads. |
| Start/stop, enter while playing | `button PLAY`, `button SHIFT down; button SESSION_VIEW down; …up` | state UI = `generative_perform`, OLED mirror |
| Hat choke | pads / Generator | choke counter event; audio needs line-in capture |
| Load timing | `button LOAD` + select | `load_done ms=` event (log timestamps, `log.cpp:43`) |
| SAVE in Perform | `button SAVE` + confirm | OLED mirror, `save_done` event, JSON `dir` of `/SONGS` |

Still needs a human: flashing the first `ENABLE_SYSEX_LOAD|ENABLE_SYSEX_TEST` build from SD, enabling the dev-sysex key once, power-cycling after a crash, and any listening test. After that, `loadfw` (RAM) plus the test port let an agent rebuild, upload and run the ticket unattended. The same scripts run first in delugemu.
