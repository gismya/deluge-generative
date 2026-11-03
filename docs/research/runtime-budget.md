# Runtime budget on the Deluge

Research for [#3](https://github.com/gismya/deluge-generative/issues/3), part of the map [#1](https://github.com/gismya/deluge-generative/issues/1).

**Question.** What CPU, RAM and voice budget does the Generator have on the Deluge (RZ/A1L, v1.3.0-rc3)? What is safe to do per sequencer tick, in the audio routine, and in the UI loop? How much heap or SDRAM is free for Generator state? What limits does the SD card put on reading presets and samples mid-performance?

**Sources.** The firmware source on the `generative` branch (commit `3ca05672`, upstream `v1.3.0-rc3` plus agent docs), a release build of that commit made for this note, the community manual pages under `website/`, and the Renesas product page. File references are relative to the repo root. Numbers marked **(estimate)** are derived by me, not stated anywhere.

---

## 1. Hardware and clocks

| Item | Value | Source |
|---|---|---|
| CPU | Arm Cortex-A9, single core, 400 MHz | `src/RZA1/peripheral_init_basic.c:96-107` (`FRQCR = 0x1035`, "I Clock = 400.00MHz"); [Renesas RZ/A1L page](https://www.renesas.com/en-us/products/microcontrollers-microprocessors/rz/rza/rza1l.html) |
| Other clocks | G 266.67 MHz, B 133.33 MHz, P1 66.67 MHz, P0 33.33 MHz | same lines |
| On-chip RAM | 3 MB at `0x20000000-0x202FFFFF` | `linker_script_rz_a1l.ld:77`; `general_memory_allocator.h:39-41` |
| SDRAM | 64 MB at `0x0C000000-0x0FFFFFFF` (the linker comment says 32 MB, but `LENGTH = 0x04000000` is 64 MB) | `linker_script_rz_a1l.ld:65`; `src/RZA1/cpu_specific.h:122-123` |
| Caches | 32 KB L1 I + 32 KB L1 D, 128 KB L2 | Distributor listing only, not checked against the Renesas datasheet |
| Audio | 44.1 kHz, 128-sample stereo TX ring buffer | `src/definitions_cxx.hpp:1044`; `src/definitions.h:54` |

Derived **(estimate)**: the TX ring holds 128 / 44100 = **2.90 ms** of audio. That is about 1.16 M CPU cycles per 128 samples, or about 9,070 cycles per output sample.

## 2. Execution model: one core, cooperative scheduler

- Release builds compile with `-DUSE_TASK_MANAGER` (`CMakeLists.txt:193`). `deluge_main()` then calls `registerTasks(); startTaskManager();` instead of the old `mainLoop()` (`src/deluge/deluge.cpp:923-928`).
- The scheduler is **cooperative and non-preemptive**. `TaskManager::start()` loops: pick a task, run it to completion, repeat (`src/OSLikeStuff/task_scheduler/task_scheduler.cpp:343-372`). Only hardware ISRs preempt (MIDI/gate output timer, SD/DMA completion and so on).
- The audio routine is **not an interrupt**. It is task priority 0, with min/target/max intervals of 8/64/128 samples (0.18 / 1.45 / 2.90 ms) (`src/deluge/deluge.cpp:551-552`).
- `chooseBestTask()` runs the least important task whose *average* duration fits before a more important task is due. A task that is overdue past its `maxInterval` is forced (`task_scheduler.cpp:51-119`). Because nothing is preempted, **one long task delays the audio routine for its whole run**.
- The scheduler logs `Task %s took too long` for any run over **3 ms** (`task_scheduler.cpp:257-259`). The audio routine logs when it was last called more than 3 ms ago (`src/deluge/processing/engines/audio_engine.cpp:733-740`).
- `yield(until, timeout)` lets a long task pause and re-enter the scheduler loop **on the same stack** (`task_scheduler.cpp:284-337`). Nothing is context-switched. The yielding task's frames stay on the stack while other tasks run on top of them.
- At most `kMaxTasks = 25` tasks (`task_scheduler.h:18`). About 17 are registered at boot (`deluge.cpp:531-611`). Priority gaps 10/20/30/40 are reserved for dynamic tasks (`deluge.cpp:544`).

Registered tasks (priority: lower is more important) (`deluge.cpp:550-607`):

| Pri | Task | Target / max interval |
|---|---|---|
| 0 | audio routine | 1.45 / 2.90 ms |
| 1 | check USB MIDI | 0.5 / 1 ms |
| 2 | interpret encoders | 1 / 2 ms |
| 3 | playback routine (analog clock input only) | 1 / 2 ms |
| 4 | MIDI routine (incoming USB/serial MIDI) | 1 / 2 ms |
| 5 | OLED/CV handshake | 0.5 / 1 ms |
| 6 | load clusters (SD sample streaming) | 0.1 / 0.2 ms |
| 7 | audio slow | 5 / 50 ms |
| 8 | buttons and pads | 5 / 10 ms |
| 11 | pending UI rendering | 10 / 30 ms |
| 12 | SysEx | 0.2 / 10 ms |
| 21-23 | audio file slow, audio recorder slow, playback slow (undo/redo) | 90-100 / 100-200 ms |
| 31-32 | PIC flush, UI timer routine | 1 / 20 ms, 0.7 / 10 ms |

## 3. Where the sequencer tick runs

- **Sequencer ticks run inside the audio routine.** `routine_()` calls `tickSongFinalizeWindows(numSamples)` before `renderAudio()` (`audio_engine.cpp:791-795`). That function calls `playbackHandler.actionTimerTick()` / `actionSwungTick()` when a tick falls at the start of the window. If a tick falls inside the window, it shortens the window so the tick lands exactly on a window boundary, which makes ticks sample-accurate (`audio_engine.cpp:926-970`).
- `actionSwungTick()` calls `currentPlaybackMode->doTickForward()`, which walks clips and note rows and issues note-ons (`src/deluge/playback/playback_handler.cpp:968, 1045`). So **any work done per tick is charged directly to the audio routine's time**.
- Ticks are **event-driven**, not per-PPQN. The handler jumps `swungTicksTilNextEvent` ahead to the next thing that needs doing (`playback_handler.cpp:695-718, 968-976`). Idle ticks cost almost nothing.
- Resolution: bar length = `96 << inputTickMagnitude` (`src/deluge/model/song/song.cpp:5776-5778`), with default magnitude 2 (`src/deluge/storage/flash_storage.cpp:350`). That gives **384 ticks/bar = 96 PPQN** by default. Derived **(estimate)**: one tick lasts 10.4 ms at 60 BPM, 5.2 ms at 120 BPM and 3.5 ms at 180 BPM (about 230 samples at 120 BPM).
- `InstrumentClip::processCurrentPos` gathers pending note-ons in a static list capped at `kMaxNumNoteOnsPending = 64` per clip per tick (`src/definitions_cxx.hpp:274`; `src/deluge/model/clip/instrument_clip.cpp:720-731`).
- Under `USE_TASK_MANAGER`, incoming MIDI and analog clock are handled by their own tasks (priority 3-4), not in the audio routine (`audio_engine.cpp:741-746`).

## 4. CPU budget and voice culling

There is **no fixed global voice count**. Voices are limited by CPU, through culling:

- `numSamplesLimit = 80`, `direnessThreshold = 50` (`audio_engine.cpp:127-129`). `setDireness()` turns the scheduler's **average audio-task runtime** into samples (`getAverageRunTimeForTask(routine_task_id) * 44100`) and compares it against these limits (`audio_engine.cpp:516-560`).
- Derived **(estimate)**: direness (which degrades some quality choices, for example sinc to linear interpolation per the manual) starts when the audio task averages about **50 samples = 1.13 ms** per call. Culling starts at about **80 samples = 1.81 ms**, plus 20 samples for each voice started in that render (`audio_engine.cpp:445`).
- Cull steps (`audio_engine.cpp:440-512`): 0-31 samples over the limit and rising means one soft release. 32 or more over means `over >> 4` voices, split between immediate kill, fast release and one soft release. Culling stops at `MIN_VOICES = 7` (`audio_engine.cpp:134`) unless 40 or more samples over. Victims are picked by `getPriorityRating()` across all sounds, and per-sound `voicePriority` (LOW/MEDIUM/HIGH) feeds into that.
- **Voice-start rate limit**: at most **4 new voices per render window**, or 1 when `cpuDireness > 12` (`audio_engine.cpp:1622-1629`). Extra note-ons stay pending and start on later renders (`src/deluge/processing/sound/sound.cpp:1551-1563, 2368-2370`). A burst of simultaneous notes from the Generator gets spread over several windows of up to 2.9 ms each.
- **Per-sound polyphony**: `maxVoiceCount` defaults to **8** (`src/deluge/processing/sound/sound.h:121`, saved as `maxVoices`). Going over it terminates that sound's oldest active voice (`sound.cpp:4903-4907`).
- **Voice memory**: `Voice`, `VoiceSample` and `TimeStretcher` come from object pools (`audio_engine.h:142-144`). Each pool pre-allocates **48** objects (`src/deluge/memory/object_pool.h:30`) and falls back to the allocator when empty (`object_pool.h:98-108`). If that allocation fails, the sound steals one of its own voices (`sound.cpp:4909-4920`).
- Community-measured voice capacity (manual, `website/src/content/docs/manual/device_overview/cpu_and_memory.mdx:50-57`): about 100 FM/subtractive voices without filters, about 60 "normal" synth voices, about 30 DX7 voices, about 30 wavetable or very short sample voices, and **about 24 mono / 12 stereo streamed samples "with a good SD card"**. Line 11 also says simple synth voices allow "around 64" and unaffected sample voices up to 110. The manual adds a per-sound overhead "even if it is not sounding" that "may begin to affect CPU performance if you have more than 50 to 100 sounds loaded" (line 21).

What is left for non-audio work, all **(estimate)**: when the audio task sits just under the cull threshold (about 1.8 ms per call, roughly every 1.45-2.9 ms), the other tasks share about 1 ms or less between audio calls. A lightly loaded song leaves much more. Neither the source nor the docs give a measured "spare CPU %". The scheduler can print per-task load, duration and latency when built with `SCHEDULER_DETAILED_STATS` (`task_scheduler.cpp:404-440`, printed every 10 s via `D_PRINTLN`). That is the tool to measure it on hardware.

## 5. Memory map and what is free

Allocator layout (`src/deluge/memory/general_memory_allocator.cpp:42-78`, sizes from `general_memory_allocator.h:31-33`). Measured from a release build of `3ca05672` (`arm-none-eabi-size`/`nm` on `build/Release/deluge.elf`, toolchain v22):

| Region | Address range (this build) | Size | Who allocates there |
|---|---|---|---|
| internal (general) | `__heap_start 0x201E5754` to `program_stack_start 0x202F8000` | **1,124,524 B ≈ 1.07 MiB** at boot | `allocMaxSpeed`, `malloc` (internal first) |
| internal small (<128 B) | `0x20005250-0x20020000` | 110,000 B ≈ 107 KiB | small `allocMaxSpeed`/`malloc` requests |
| external (SDRAM) | top of SDRAM minus 2 MiB + 128 KiB | **2 MiB** (`RESERVED_EXTERNAL_ALLOCATOR`) | global `operator new`, `allocExternal`, overflow from `allocMaxSpeed`/`allocLowSpeed` |
| external small (<128 B) | top 128 KiB of SDRAM | 128 KiB | small `new` |
| stealable (SDRAM) | `__sdram_bss_end 0x0C033910` to the external region | **64,669,424 B ≈ 61.7 MiB** | sample/cache clusters (evictable). Non-stealable allocations land here only as a last resort |
| program stack | `0x202F8000-0x20300000` | **32 KiB** | the main stack, shared by every task and every nested `yield` |

Static footprint of this build: `.text` 1,178,408 B + `.rodata` 419,708 B + `.data` 9,984 B + `.bss` 182,408 B in internal RAM. Code executes from on-chip RAM. Static data in SDRAM (`.sdram_*`) is 211,216 B. The largest static object is `AudioEngine::reverb` (≈128 KiB, internal `.bss`).

Allocator facts that matter for Generator state:

- **Global `operator new` allocates only from the 2 MiB external region** and throws `BAD_ALLOC` when it is full. It does not fall back (`src/deluge/memory/operators.cpp:7-18`). `std::vector`/`std::string` with the default allocator draw from the same 2 MiB pool as every other `new`.
- `malloc()` maps to `delugeAlloc(size, true)`, internal first (`src/malloc.c:7-8`).
- `alloc()` tries internal (if allowed), then external, then **the stealable region**. Using the stealable region evicts cached sample clusters ("Dire memory, resorting to stealable area", `general_memory_allocator.cpp:172-218`). A big non-stealable allocation therefore competes with sample streaming.
- Header comments saying the external region is "8MiB" (`src/deluge/memory/sdram_allocator.h:13`, `external_allocator.h:13`) and "200k" (`general_memory_allocator.h:32-33`) are stale. The constants are 2 MiB, 128 KiB and 64 KiB.
- The allocator is **not re-entrant**. Re-entry while locked returns `nullptr` (`general_memory_allocator.cpp:121-124, 175-178`). Allocation searches an ordered empty-space list and may run `Stealable` eviction callbacks, so its time cost is not bounded.
- Songs and clips use `allocMaxSpeed` (`deluge.cpp:383`, `load_song_ui.cpp:357`), so **song data competes with the Generator for the ~1.07 MiB internal heap**. That figure is free *at boot*. How much a typical song leaves free is not documented and needs a hardware measurement. The manual says "64MB ... would be enough to hold over 2 million notes" (`cpu_and_memory.mdx:42`).
- `checkStack()` freezes with `E338` when less than 200 B of the 32 KiB stack remain (`general_memory_allocator.cpp:83-93`). It runs from the audio routine and from every `yield()`.

## 6. SD card: streaming, presets, songs

- **Bus**: SDHI with DMA, high-speed mode, 4-bit bus (`src/RZA1/sdhi/inc/sd_cfg.h:50-98`). FatFs without exFAT (`src/fatfs/ffconf.h:234`), so FAT32 only. The manual recommends 32 kB clusters (`website/src/content/docs/manual/device_overview/sd_card.mdx:58`).
- **Cluster size** is the card's FAT cluster size, fixed at boot (`src/deluge/storage/audio/audio_file_manager.cpp:77-96`; default 32,768 B in `src/deluge/storage/cluster/cluster.cpp:30`). Samples are read and cached one cluster at a time. Each playing sample keeps `kNumClustersLoadedAhead = 2` clusters ahead (`src/definitions_cxx.hpp:679`).
- **Streaming happens in a task, never in the audio routine.** The "load clusters" task (priority 6) calls `loadAnyEnqueuedClusters()` (`deluge.cpp:572`). That function returns immediately while `audioRoutineLocked` is set (`audio_file_manager.cpp:1280-1282`).
- **SD waits yield.** Every SD wait (command interrupt, DMA end, power-up timer) goes through `yieldingRoutineForSD` / `yieldingRoutineWithTimeoutForSD`, so audio and other tasks keep running during card I/O (`src/deluge/drivers/sd/sd.c:47-82`; `src/RZA1/sdhi/userdef/sd_dev_low.c:1115`; `deluge.cpp:939-960`). Nested SD access spins instead (`sdRoutineLock`). DMA timeouts are at least 2 s, because "block write operations sometimes just randomly take as long as 1250ms" (`sd_dev_low.c:1114`).
- **Measured stream counts** (manual, `sd_card.mdx:62-79`): Kingston Canvas Go! Plus manages 9-12 stereo streams, SanDisk Extreme Pro 6-8, and the stock Verbatim card **3-7**. What limits it is random-access latency, not bus speed (`sd_card.mdx:62`). Derived **(estimate)**: one 16-bit stereo stream is about 176 KB/s, so 12 streams are about 2.1 MB/s.
- Samples that fit in RAM are cached as stealable clusters and need no SD reads until evicted. Only long or evicted samples stream.
- **Loading a song while playing is a stock feature.** The next song is parsed into a second `Song`, only its "crucial" samples load first, then the swap is armed. The code waits up to 5 s for the queue (`src/deluge/gui/ui/load/load_song_ui.cpp:440-505`). The manual warns the new song may "fail to play some sounds for the first couple of seconds" when both songs are sample-heavy (`cpu_and_memory.mdx:44`).
- **Presets** are XML files parsed on the UI side through `StorageManager::loadInstrumentFromFile` (`src/deluge/storage/storage_manager.cpp:326`, called from `load_instrument_preset_ui.cpp:636, 941`). The file read buffer is one cluster, allocated with `allocLowSpeed(32768 + …)` (`storage_manager.cpp:791`). Parse time per preset is not documented.

## 7. What is safe where (constraints, not a design)

These follow from the facts above. The microsecond figures are **estimates**.

**In a sequencer tick (inside the audio routine):**
- Everything here counts against the 1.13 ms direness and 1.81 ms cull thresholds. Each 10 µs is about 4,000 cycles and about 0.44 samples.
- No SD access. Card loads are blocked while `audioRoutineLocked` is set, and a yield from inside the audio routine cannot run the audio task, so the audio buffer drains.
- Avoid allocator calls if possible. They are unbounded, can evict sample clusters, and return `nullptr` on re-entry.
- More than 4 note-ons in one window are deferred to later renders. More than 64 note-ons per clip per tick are not supported.

**In the audio render itself:** only DSP for sounding voices. The Generator does not belong here.

**In a scheduler task (UI-level work):**
- Keep each run well under 3 ms (the warning threshold), and in practice around 1 ms or less when audio is heavily loaded. The audio task is forced once it is 2.9 ms overdue, but only *between* task runs.
- Longer jobs (parsing, planning a section) must be split into chunks or call `yield()`.
- `yield()` nests on the 32 KiB stack and lets the audio routine, and therefore sequencer ticks, run in the middle of the job. State the tick reads must not be left half-updated across a yield.
- No locking is needed between a task and the tick path, because both run on one cooperative core. ISR-shared state is the exception.

## 8. Open items this research could not settle

- How much internal heap and how much of the 2 MiB external region are free with a realistic song loaded. This needs a hardware or emulator measurement. The allocator has no free-space query in release builds.
- The real cost of `doTickForward()` for a dense song, and how much spare CPU stays between audio calls. Needs `SCHEDULER_DETAILED_STATS` on hardware.
- XML preset parse time and song-load time on typical cards.
- The datasheet values for L1/L2 cache sizes and SDRAM bus width and speed (only secondary sources found).
