# delugemu and v1.3.0-rc3

Research for [#4](https://github.com/gismya/deluge-generative/issues/4), map [#1](https://github.com/gismya/deluge-generative/issues/1). Read-only: nothing was built or run. The hands-on check is [#9](https://github.com/gismya/deluge-generative/issues/9).

Researched 2026-11-03.

## Questions

1. Does delugemu run v1.3.0-rc3 community firmware builds?
2. How do you build a firmware image and load it?
3. Which limitations matter for testing a Generator (audio timing, MIDI, SD card, display, input)?
4. Can it run headless or scripted, for automated tests or CI?

## Short answer

- **Use [SynthstromAudible/delugemu](https://github.com/SynthstromAudible/delugemu), not [gramster/delugemu](https://github.com/gramster/delugemu).** The community adopted Graham Wheeler's emulator in October 2026. The SynthstromAudible repo contains all of gramster's history plus about 30 further PRs (#1-#28, 2026-10-18 to 2026-11-01), including gramster's four open PRs. gramster's repo was last pushed 2026-08-06. Source: firmware blog post [`website/src/content/docs/blog/2026-10-26-first-post.md`](https://github.com/gismya/deluge-generative/blob/c0725ce56d9a9e1e3cca10a0cc2cf6750594c7e7/website/src/content/docs/blog/2026-10-26-first-post.md) ("We've adopted Graham Wheeler's deluge emulator ... and are trying to get it as accurate as possible") and both repos' commit logs.
- **v1.3.0-rc3 is not named anywhere in either repo**, but very likely runs. SynthstromAudible/delugemu's CI boots the rolling DelugeFirmware `beta` prerelease (built from `main`) on every push and nightly, and all recent runs pass. `main` carries the same OLED handshake rewrite that rc3 backported. gramster's repo was verified against v1.3.0-beta (July 2026) and 1.2.x. Details under [Firmware versions](#firmware-versions-known-to-run).
- **Build**: `./dbt build release` (or `debug`) gives `build/Release/deluge.elf` and `deluge.bin`. delugemu loads either with `-kernel`; prefer the unstripped `.elf`. In a git worktree `./dbt` refuses to run unless `DBT_NO_SYNC=1` is set.
- **Headless/scripted/CI: yes, and well supported.** There is a stdlib-only Python library (`pylib/delugemu`, `DelugeSession`), a CLI, JSON/YAML scenario files, a GitHub composite action, deterministic virtual-time runs (`-icount ... sleep=off` with `run_until`), and read-back of OLED, LEDs and pads through QMP `qom-get`.
- **Main limitation for Generator testing: audio and timing are behavioural, not cycle-accurate.** On a fast host the emulated CPU runs faster than a Deluge; on a slow one, audio breaks up. Most peripherals other than the SD card respond instantly. Use the perf plugin (`timectl=1,cpi=1.45`) when you need "Deluge speed".

## Which delugemu

| | gramster/delugemu | SynthstromAudible/delugemu |
|---|---|---|
| Last push | 2026-08-06 (`95e3b70`) | 2026-11-02 (`23d1e74`) |
| Relationship | Original | Same history plus PRs #1-#28 (not a GitHub fork) |
| Issues | 1 open ([#6](https://github.com/gramster/delugemu/issues/6), Linux input) | Issues disabled |
| Releases | v0.3.0-v0.5.7 (last 2026-07-24) | Rolling `nightly` prerelease (macOS arm64 DMG/tar.gz, Linux x86_64, Windows MSI/zip) |
| Scripting library | No (QMP helper scripts only) | `pylib/delugemu` + CLI + scenarios |
| MTU2 timer | Every channel counts at P0φ, so slow-timer waits run 1024x too short ([SA PR #13](https://github.com/SynthstromAudible/delugemu/pull/13)) | Prescaler, compare-match and IRQs modelled |
| SD card | Instant | Timed like a real card; 32 KiB-cluster FAT32 images, minimum 4 GiB sparse ([SA PR #9](https://github.com/SynthstromAudible/delugemu/pull/9), [#11](https://github.com/SynthstromAudible/delugemu/pull/11)) |
| Flash settings | SPIBSC reads return 0, so settings load as all-zero | Erased flash reads 0xFF, so factory defaults load ([SA PR #7](https://github.com/SynthstromAudible/delugemu/pull/7)) |
| PIC OLED flag | Version byte reports no OLED | Reports OLED when a panel is wired |

Everything below refers to SynthstromAudible/delugemu at `23d1e74` unless it says otherwise. The emulator is a custom QEMU machine (`-M deluge`) built against a pinned QEMU v11.0.1 submodule plus local patches (`qemu-patches/0001`-`0007`) ([README](https://github.com/SynthstromAudible/delugemu/blob/23d1e7455028e393f2b1c08b9f303b4481de5d44/README.md), [`.gitmodules`](https://github.com/SynthstromAudible/delugemu/blob/23d1e7455028e393f2b1c08b9f303b4481de5d44/.gitmodules)).

## Firmware versions known to run

Evidence, from strongest to weakest:

1. **SynthstromAudible/delugemu CI** ([`.github/workflows/build.yml`](https://github.com/SynthstromAudible/delugemu/blob/23d1e7455028e393f2b1c08b9f303b4481de5d44/.github/workflows/build.yml), [`.github/actions/test-assets/action.yml`](https://github.com/SynthstromAudible/delugemu/blob/23d1e7455028e393f2b1c08b9f303b4481de5d44/.github/actions/test-assets/action.yml)) runs `tests/run-all.sh --require-firmware` against:
   - `latest`: the newest stable DelugeFirmware release, currently `release_1_2_1` (Chopin). Runs on Linux, macOS and Windows.
   - `beta`: the rolling DelugeFirmware `beta` prerelease. Linux only, `continue-on-error` so it does not block merges. On 2026-11-03 the `beta` tag pointed at `f649f7fa` on DelugeFirmware `main`.

   The last week of `build` and `nightly` runs all concluded `success` (`gh run list -R SynthstromAudible/delugemu`).
2. **v1.3.0-rc3 against `main`**: rc3 branched from `main` at `2ce19522` (2026-10-15) and has 37 release-branch commits. `main` (the `beta` build) has 96 more. The OLED handshake rewrite is on `main` as `4d38a676` (#5151) and on the release branch as `eede93c6` plus the conflict fix `c0725ce5`. So the riskiest 1.3 change is exercised by the `beta` CI leg.
3. **gramster commit [`f9cde98`](https://github.com/gramster/delugemu/commit/f9cde98)** (2026-07-23): "Verified MATCH against ELF symbol truth on v1.3.0-beta (twice) and official 1.2.x." This is a render-head detection fix. The beta firmware booted and played audio.
4. **SA PRs [#13](https://github.com/SynthstromAudible/delugemu/pull/13), [#18](https://github.com/SynthstromAudible/delugemu/pull/18), [#23](https://github.com/SynthstromAudible/delugemu/pull/23)** each report "stock" / "main's firmware" giving identical panels before and after the change. These PRs also add support for an experimental Rust/Embassy BSP firmware.
5. **SA PR [#22](https://github.com/SynthstromAudible/delugemu/pull/22)** (open) adds integration tests for an unreleased firmware feature (arp freeze). They skip unless `ARP_FREEZE_FIRMWARE` points at a build that has the feature. This is the pattern Generator tests could copy.
6. **The firmware references delugemu directly.** [`tests/hardware/oled_cv/README.md`](https://github.com/gismya/deluge-generative/blob/c0725ce56d9a9e1e3cca10a0cc2cf6750594c7e7/tests/hardware/oled_cv/README.md) (added with #5150, which rc3 contains) says: "delugemu currently transmits UART bytes immediately and cannot reproduce the physical wire delay of a grid redraw."

The run.sh auto-download offers only 1.2.1 ([`scripts/run.sh` L257-258](https://github.com/SynthstromAudible/delugemu/blob/23d1e7455028e393f2b1c08b9f303b4481de5d44/scripts/run.sh#L257-L258)). This is a default, not a compatibility limit.

## The 1.3 OLED handshake and delugemu

rc3 commits `eede93c6` (backport of #5151 "Service OLED handshakes independently of UI input", marked BACKPORT-CONFLICT) and `c0725ce5` ("Resolve OLED handshake backport conflict for release 1.3") change how the firmware waits for the PIC to acknowledge OLED SELECT/DESELECT. The conflict fix itself only touches `registerTasks()` in `src/deluge/deluge.cpp`: it keeps release/1.3's `loadAnyEnqueuedClusters(128, false)` and adds the new "OLED/CV handshake" task.

What changed in the firmware (all in the fork at `c0725ce5`):

- The firmware no longer waits for `readButtonsAndPads()` to see the 248/249 echo and arm a 3 ms UI timer. A dedicated high-priority task, `oledRoutine(); PIC::flush();` ([`deluge.cpp` L562-570](https://github.com/gismya/deluge-generative/blob/c0725ce56d9a9e1e3cca10a0cc2cf6750594c7e7/src/deluge/deluge.cpp#L562-L570)), now scans the PIC RX DMA ring for the echo ([`oled_low_level.c` L97-122](https://github.com/gismya/deluge-generative/blob/c0725ce56d9a9e1e3cca10a0cc2cf6750594c7e7/src/RZA1/oled/oled_low_level.c#L97-L122)).
- The scan reads the DMA write position from `DMACn(ch).CRDA_n`. It overwrites matched acknowledgement bytes with 255 (`OLED_MESSAGE_ACK_CONSUMED`) through the uncached mirror (`+0x40000000`) and moves the remaining panel bytes into a 256-byte staging queue ([`uart.c` L244-271](https://github.com/gismya/deluge-generative/blob/c0725ce56d9a9e1e3cca10a0cc2cf6750594c7e7/src/deluge/drivers/uart/uart.c#L244-L271), new [`rx_buffer.h`](https://github.com/gismya/deluge-generative/blob/c0725ce56d9a9e1e3cca10a0cc2cf6750594c7e7/src/deluge/drivers/uart/rx_buffer.h)).
- The 3 ms settle and 50 ms retry are measured on the MTU2 slow timer (`*TCNT[TIMER_SYSTEM_SLOW]`).
- A SELECT is held back while fewer than 8 bytes of PIC RX space remain ([`oled_low_level.c` L132](https://github.com/gismya/deluge-generative/blob/c0725ce56d9a9e1e3cca10a0cc2cf6750594c7e7/src/RZA1/oled/oled_low_level.c#L132)).

What delugemu provides (code read only, not run):

- **Echo**: the PIC model echoes 248/249 immediately on SELECT/DESELECT ([`deluge_pic.c` L509-527](https://github.com/SynthstromAudible/delugemu/blob/23d1e7455028e393f2b1c08b9f303b4481de5d44/src/hw/misc/deluge_pic.c#L509-L527)).
- **DMA ring**: `rza1l_dmac_peripheral_rx_push` writes each byte at `CRDA` and advances it, so a firmware read of `CRDA_n` sees the live write position ([`rza1l_dmac.c` L739-756](https://github.com/SynthstromAudible/delugemu/blob/23d1e7455028e393f2b1c08b9f303b4481de5d44/src/hw/dma/rza1l_dmac.c#L739-L756)).
- **Uncached mirrors**: SRAM and SDRAM are aliased at `+0x40000000`. QEMU models no cache, so the alias is coherent with the ring ([`rza1l_soc.c` L113](https://github.com/SynthstromAudible/delugemu/blob/23d1e7455028e393f2b1c08b9f303b4481de5d44/src/hw/arm/rza1l_soc.c#L113)).
- **MTU2**: SA PR #13 makes the counters honour the prescaler, so the 3 ms / 50 ms waits are realistic. On gramster's model they would run about 1024x short, which shortens the waits but should not deadlock.
- **OLED detection**: v1.3 decides `have_oled` from DMA channel 4's `CHCFG`, which the bootloader leaves set ([`deluge.cpp` L653](https://github.com/gismya/deluge-generative/blob/c0725ce56d9a9e1e3cca10a0cc2cf6750594c7e7/src/deluge/deluge.cpp#L653)). delugemu seeds that register on reset ([`rza1l_dmac.c` L120, L945](https://github.com/SynthstromAudible/delugemu/blob/23d1e7455028e393f2b1c08b9f303b4481de5d44/src/hw/dma/rza1l_dmac.c#L120)), so the firmware takes the OLED path.

Conclusion: delugemu provides everything the new handshake reads, and the `beta` CI leg already exercises the same handshake code. One part is not exercised: the backpressure path. delugemu's UART is instantaneous, so the RX ring never fills the way a real pad redraw fills it. The firmware's own README says so (above). A handshake regression in our changes would only show up there on hardware.

## Building an image delugemu accepts

Firmware side (this repo, v1.3.0-rc3):

- `./dbt build release` or `./dbt build debug` (no argument builds both). Output goes to `build/<Config>/` ([getting_started.md](https://github.com/gismya/deluge-generative/blob/c0725ce56d9a9e1e3cca10a0cc2cf6750594c7e7/website/src/content/docs/development/deluge/getting_started.md), [`scripts/tasks/task-build.py`](https://github.com/gismya/deluge-generative/blob/c0725ce56d9a9e1e3cca10a0cc2cf6750594c7e7/scripts/tasks/task-build.py)). `relwithdebinfo` also exists.
- With `ENABLE_VERSIONED_OUTPUT` off (the default), files are `deluge.elf`, `deluge.bin`, `deluge.hex`, `deluge.map` and `deluge.nmdump` ([`CMakeLists.txt` L55-61, L259-314](https://github.com/gismya/deluge-generative/blob/c0725ce56d9a9e1e3cca10a0cc2cf6750594c7e7/CMakeLists.txt#L55-L61)). The ELF is unstripped even for Release: "Enabled to generate a full ELF, objcopy will do the stripping". The `.bin` is `objcopy -O binary -S`.
- The first `./dbt` run downloads the toolchain (v22, about 1.3 GB) into `toolchain/`.
- **Worktrees**: `./dbt` exits with `".git" directory not found` unless `DBT_NO_SYNC=1` is set, because in a worktree `.git` is a file ([`dbt` L28-33](https://github.com/gismya/deluge-generative/blob/c0725ce56d9a9e1e3cca10a0cc2cf6750594c7e7/dbt#L28-L33)). delugemu's own skill gives the recipe: `DBT_NO_SYNC=1 DBT_TOOLCHAIN_PATH=<main checkout> ./dbt build release`, which reuses one toolchain download ([`measure-firmware-performance/SKILL.md`](https://github.com/SynthstromAudible/delugemu/blob/23d1e7455028e393f2b1c08b9f303b4481de5d44/.claude/skills/measure-firmware-performance/SKILL.md)). This repo has no submodules, so skipping the sync costs nothing.

Emulator side ([`src/hw/arm/deluge.c` `deluge_load_firmware`](https://github.com/SynthstromAudible/delugemu/blob/23d1e7455028e393f2b1c08b9f303b4481de5d44/src/hw/arm/deluge.c#L82)):

- `-kernel <file>`; `run.sh` passes its first argument through.
- An ELF is loaded by program headers, with the entry from the header.
- Otherwise it is treated as a raw Deluge image. It checks the `.BootLoad_ValidProgramTest.` signature at offset 0x2c, then copies to `code_start` and jumps to `code_execute`. Both must lie inside the 3 MB on-chip SRAM.
- **Use the `.elf`.** `--tx-render-head auto` resolves `AudioEngine::i2sTXBufferPos` exactly from its symbols (with a `.bin` it falls back to a heuristic RAM scan), GDB gets source-level symbols, and the perf tools need it.
- Debug is `-Og` without LTO; Release is `-O2 -flto`. Use Release for anything timing- or CPU-load-related, since that is what ships ([`docs/firmware-perf.md`](https://github.com/SynthstromAudible/delugemu/blob/23d1e7455028e393f2b1c08b9f303b4481de5d44/docs/firmware-perf.md)).

Typical invocation:

```sh
# from a delugemu checkout (built with scripts/bootstrap.sh, integrate.sh, build.sh)
./scripts/mksd.sh sdcard build/deluge_sd.img          # FAT32, 32 KiB clusters, >= 4 GiB sparse
./scripts/run.sh /path/to/build/Release/deluge.elf --sd build/deluge_sd.img
# or the prebuilt nightly app bundle:
/Applications/DelugEmu.app/Contents/MacOS/DelugEmu /path/to/deluge.elf --sd card_rw
```

## Limitations that matter for a Generator

**Audio timing**

- QEMU's TCG engine is not cycle-accurate. By default the guest runs as fast as the host allows, while the audio device consumes on wall-clock time ([README "Performance"](https://github.com/SynthstromAudible/delugemu/blob/23d1e7455028e393f2b1c08b9f303b4481de5d44/README.md)).
- On a fast host (the docs cite an M4 Pro) audio keeps up but the CPU looks less loaded than on a Deluge. Under heavy synthesis on a slow host the result is gaps (render-head clamp, on by default) or slow but clean audio (`--icount`).
- The default output cushion is about 15 ms (`--audio-buffer`). Raising it adds latency from MIDI or pad to sound.
- For realistic CPU load, use the `dzcache` TCG plugin with `timectl=1,cpi=1.45`, calibrated against hardware PMU counters ([`tools/perf/README.md`](https://github.com/SynthstromAudible/delugemu/blob/23d1e7455028e393f2b1c08b9f303b4481de5d44/tools/perf/README.md), SA PRs [#20](https://github.com/SynthstromAudible/delugemu/pull/20), [#21](https://github.com/SynthstromAudible/delugemu/pull/21)). With it, Delay, reverb and filters dominate a loaded song and the scheduler is about 10%. Load questions about a Generator should be answered this way, or on hardware.
- A byte-exact audio diff between runs is not achievable. `-icount ... sleep=on` warps virtual time, and oscillator phases free-run ([`docs/firmware-perf.md` "Validation reality"](https://github.com/SynthstromAudible/delugemu/blob/23d1e7455028e393f2b1c08b9f303b4481de5d44/docs/firmware-perf.md)). Assert on notes, display and LED state, not on audio samples.

**MIDI**

- DIN MIDI is SCIF0, attached to any QEMU chardev with `--midi` (`udp:`, `pty`, socket, file; `coremidi` on macOS). A test can capture what a Generator sends as raw bytes.
- USB host MIDI uses `--usb-midi`, with enumeration tested by `tests/usb-midi.sh`. SA PR #1 added the USB audio peripheral.
- The README says outgoing MIDI "works well". Incoming notes are delayed by the audio cushion.
- MIDI receive timing goes through a modelled timing-capture DMA channel tied to the audio position ([`rza1l_dmac.c`](https://github.com/SynthstromAudible/delugemu/blob/23d1e7455028e393f2b1c08b9f303b4481de5d44/src/hw/dma/rza1l_dmac.c)), but byte delivery is instantaneous.
- Analog clock and trigger in can be driven programmatically ([SA PR #28](https://github.com/SynthstromAudible/delugemu/pull/28)).

**SD card**

- The card is a raw FAT32 image with a power-of-two size, or a folder snapshotted at launch. Folders whose name ends in `_rw` are written back on clean exit.
- SA times card access like a real card. Compose-style renders that save songs will therefore take realistic SD time.
- Work on a copy if the firmware writes, as the README advises.

**Display**

- The OLED (SSD130x decoder, 128x48) is modelled end to end. `read_oled()` returns the framebuffer as bools, and `oled_text()` / `assert_oled_contains()` work on it.
- The 7-segment display is a stub, which does not matter for an OLED-only target.
- Pad RGB, indicator LEDs, gold-knob LEDs and PIC pad flashes can all be read. Generator output drawn on pads can be asserted directly.

**Input**

- Buttons, pads (18x8 including the sidebar), encoder detents (Gray-code quadrature) and encoder pushes are available as raw `dz-event` writes or through the skin with mouse and keyboard.
- Chords go through `latch()` or `hold()`.
- Linux input bugs reported in gramster [#6](https://github.com/gramster/delugemu/issues/6) were fixed in SA PRs #4-#6 (gramster PRs #7-#9).

**Not modelled or instant**

- UART wire delay to the PIC.
- CV/OLED SPI bus contention. The README lists it as "in progress".
- SDRAM bus contention.
- Caches, unless the perf plugin is used.
- The SCUX and I2C blocks; the firmware does not use them.

## Headless, scripted and CI use

- **Headless**: `run.sh --display none` or `--display headless` (serial and monitor on stdio), plus `--audio none`. `DelugeSession.launch()` defaults to headless and silent ([`pylib/delugemu/session.py`](https://github.com/SynthstromAudible/delugemu/blob/23d1e7455028e393f2b1c08b9f303b4481de5d44/pylib/delugemu/session.py)).
- **Library**: `pylib/delugemu`, stdlib only ([`docs/scripting.md`](https://github.com/SynthstromAudible/delugemu/blob/23d1e7455028e393f2b1c08b9f303b4481de5d44/docs/scripting.md)). It covers:
  - input: `press`, `turn`, `pad`, `hold`, `type_text`
  - navigation: `load_song`
  - waiting: `wait_sd_idle`
  - readout: `read_oled`, `read_leds`, `read_pads`
  - checks: `assert_no_aborts` (needs `int_log`), `screenshot`
  - `DelugeSession.attach(sock)` connects to an emulator that is already running.
- **Determinism**: launch with `extra_qemu=("-icount","shift=2,sleep=off","-S")`, then step with `run_until(ns)` and inject input at exact virtual times. `tests/determinism.sh` checks that two concurrent runs give identical panels on a fully loaded host ([SA PR #14](https://github.com/SynthstromAudible/delugemu/pull/14), [#25](https://github.com/SynthstromAudible/delugemu/pull/25)). This suits a seeded Generator: same Recipe and seed should give the same pads and OLED.
- **Scenarios and CLI**: JSON or YAML step lists run with `delugemu run scenario.json`. The `.github/actions/run-scenario` composite action fetches a firmware release (configurable `firmware-repo`) and runs scenarios.
- **CI**: the action only fetches release assets. To test this fork's own builds, a workflow would build the firmware with dbt, set `DELUGE_FIRMWARE=build/Release/deluge.elf`, and call `tests/run-all.sh`, `scenario.sh` or a pytest suite. Tests skip cleanly with no firmware, and `--require-firmware` makes a missing image fail instead.
- **Bundles**: the prebuilt nightly bundles include the TCG plugins but not `pylib`. Scripted use needs a source checkout of delugemu, or `attach` to a bundle started with `-- -qmp unix:/tmp/dz_qmp.sock,server,nowait`.
- **QMP socket path**: keep it short. macOS limits `sun_path` to about 104 characters.

## Sources

- SynthstromAudible/delugemu @ [`23d1e74`](https://github.com/SynthstromAudible/delugemu/tree/23d1e7455028e393f2b1c08b9f303b4481de5d44): README, `docs/scripting.md`, `docs/firmware-perf.md`, `docs/register-coverage.md`, `tests/README.md`, `scripts/run.sh`, `src/hw/**`, `.github/**`, PRs #1-#28, nightly release.
- gramster/delugemu @ [`95e3b70`](https://github.com/gramster/delugemu/tree/95e3b70ea76a8d287623fb67795c66380736a99e): README, `docs/roadmap.md`, `src/hw/**`, commit log (notably `f9cde98`, `a23cfb4`, `cdae52f`), issue #6, PRs #1-#11, releases v0.3.0-v0.5.7.
- This fork @ `c0725ce5` (v1.3.0-rc3): `src/RZA1/oled/oled_low_level.c`, `src/deluge/deluge.cpp`, `src/deluge/drivers/uart/{uart.c,rx_buffer.h}`, `CMakeLists.txt`, `dbt`, `scripts/tasks/task-build.py`, `website/.../getting_started.md`, `website/.../2026-10-26-first-post.md`, `tests/hardware/oled_cv/README.md`.
- DelugeFirmware releases (read only): `v1.3.0-rc3` (2026-10-26, asset `v1.3.0-rc3.zip`), `beta` tag at `f649f7fa`, `release_1_2_1`.
