# Upstream candidates

These are firmware fixes, made in this fork or found while working on it, that look better than upstream's code whatever Generative mode needs. Each one should be checked for an upstream PR to the [community firmware](https://github.com/SynthstromAudible/DelugeFirmware). Upstreaming isn't a goal of this fork (see `AGENTS.md`), so nothing here is promised.

Add an entry when you fix firmware code that isn't specific to Generative mode. Give the fix, where it is, why it's better, and its status.

| Fix | Where | Why it's better | Status |
| --- | --- | --- | --- |
| Null check before reading `clip->type` when `setup()` has neither a sound nor a clip | `src/deluge/gui/ui/sound_editor.cpp:2161` (`v1.3.0`) | It reads through a null `clip`. A root UI that isn't a clip view (such as Perform) can reach this path | Not fixed yet. Planned with firmware changes F6/F7 (see [Firmware changes the design needs](https://github.com/gismya/deluge-generative/issues/43)) |
| `currentSong` → `song` in `loadInstrumentFromFile` | `src/deluge/storage/storage_manager.cpp:406` (`v1.3.0`) | Drums file their ParamManagers with `song`, but the check reads `currentSong`. Loading a KIT preset into a song that isn't current returns `FILE_CORRUPTED`, and with no current song it dereferences null. Every caller today passes the current song. Still present on upstream `main` | Not fixed in the fork. v1 doesn't need it; it belongs to the whole-card-kit future feature (see [Building Generator Kits in code](https://github.com/gismya/deluge-generative/issues/28)) |
| Bound the sysex firmware load progress bar to the pad grid (`loadProgressPad()`) | `src/deluge/io/midi/sysex.cpp:172` (`v1.3.0`), `src/deluge/io/midi/sysex_load_progress.h` | It divides by 64 KB less than the load buffer, so the last ~3% of an upload writes past `PadLEDs::image`. Builds that link live state such as `numUIsOpen` right after it crash near the end of the load ([#55](https://github.com/gismya/deluge-generative/issues/55)). Still present on upstream `main` | Fixed in the fork |
