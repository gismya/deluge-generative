# Do roster preloads lose memory?

Ticket: [Find out whether roster preloads lose memory on a Deluge](https://github.com/gismya/deluge-generative/issues/80).
The [roster preload measurements](h5-h6-roster-preload.md) saw fast heap free after teardown fall about 1.5 K per
preload, from 437 K to 426 K over seven.

## What ran

- **Hardware:** OLED Deluge, RelWithDebInfo test build loaded with `loadfw`, on `task/preload-memory-loss`. No song
  loaded, stopped.
- **Probe:** the H5 `roster` command, which builds and tears down the largest roster: 11 drum lanes, an FX kit with 2
  sample lanes and 2 synth rows, and 4 synths. After each teardown, a new `h5x` event reports each general region's
  free bytes and gaps, and how many `AudioFile`s `audioFileManager` still holds.
- **`audioclean`:** a probe command that does what `AudioFileManager::cardReinserted()` does to `AudioFile`s nothing
  uses (`numReasonsToBeLoaded == 0`): deletes them.
- **Driver:** `scripts/generative/spec_hw_tests.py preloadsoak`, 40 preloads.

## Results

| Run | Fast free after the 1st → 40th teardown | AudioFiles | Log |
|---|---|---|---|
| Seed 11 every time (the same files) | 985,976 → 985,976 bytes, identical every time | 13 → 13 | [`preload-soak-same.log`](logs/preload-soak-same.log) |
| Seeds 1–40 (new files keep turning up) | 983,904 → 959,408 bytes (−24 K; −26.6 K from boot) | 25 → 179 | [`preload-soak-new.log`](logs/preload-soak-new.log) |
| Seeds 1–40, then `audioclean` | 959,888 → **987,848** after the clean, about 0.6 K short of boot | 179 → 0 | [`preload-soak-clean.log`](logs/preload-soak-clean.log), [`preload-soak-clean-after.log`](logs/preload-soak-clean-after.log) |

- **No leak in the preload or the teardown.** With the same files every time, free memory comes back to the byte.
- **The loss is the `AudioFile` records the firmware keeps.** Each sample file seen for the first time leaves one
  record behind after the teardown: about 160 B of fast heap and 70 B of SDRAM. When a new seed picks only files
  already seen, nothing is lost: seeds 11, 24, 26, 28, 31–32 and 38–39.
- **Stock firmware frees these records only when the card is reinserted**, or when a file of the same name is
  overwritten. Otherwise they stay, as a lookup cache for the next load of the same file.
- **So the loss is bounded by how many distinct audio files Perform can pick.** The factory card's lane folders hold
  265 samples, and the synth presets add their own. Over 40 seeds the growth slowed from about 10 records a preload
  to about 2. The ceiling here is a few hundred records: roughly 45 K of fast heap and 20 K of SDRAM. A user card with
  thousands of samples in those folders would scale with it: 2,000 files is about 320 K of fast heap, more than the
  whole roster budget.
- **Deleting the unused records gets it all back**, as `audioclean` shows: fast free returns to within about 0.6 K of
  boot.

## What it means

- Perform should delete unused `AudioFile`s itself, as `cardReinserted()` does, for example after each roster
  teardown, or when the record count or fast heap free crosses a line. The price is that a sample used again later is
  found cold: read from the card, not from the cache. That's for
  [§10.7](../generative/spec/10-sounds.md#107-when-sounds-load) and the fast heap reserve in
  [§20](../generative/spec/20-budgets.md).
- Not a firmware bug: it's stock caching that only grows unbounded under Perform's pattern of loading many different
  samples over a session. Nothing for `docs/upstream-candidates.md`.
