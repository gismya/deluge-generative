# Perform plays notes through a Generator hook, not through clips

In Perform, the Generator decides each 16th step inside the audio tick. It sends notes straight to each part's Output from a single hook in `Session::doTickForward`. It doesn't write them into the song's clips for normal playback to play. Writing them into clips would make Capture almost free, but `NoteVector` inserts allocate from inside the audio routine (`allocMaxSpeed` returns null while its lock is held), and clips would grow without limit or need trimming behind the playhead. Clip playback would also put the firmware's clip machinery between the Generator and the sound. Perform's clips therefore stay empty: one always-active, looping 1-bar clip per part, there only so the Output renders and so kit lanes have rows. Capture will have to record the Generator's event stream instead. Decision: [Generator scheduling](https://github.com/gismya/deluge-generative/issues/19).

## Considered options

- **A per-Output hook, arpeggiator-style** (`Output::doTickForwardForArp`): this touches `SoundInstrument` and `Kit`, and it computes each part on its own, which breaks the interplay order between parts.
- **Writing notes into the playing clips one step ahead**: rejected for the reasons above.
