// The rough render (spec §21.2, §21.5 step 1): an offline, deterministic render of a Recipe through the harness to a
// WAV, a Standard MIDI File with one track per part, and a text piano roll. The voices are our own small stand-ins,
// not the firmware's DSP. Renders are made locally and never committed.

#pragma once

#include "generative/core/event.h"
#include "generative/core/pitch.h"
#include "generative/core/recipe.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace harness::render {

/// What the Generator played on one decided step that the event stream doesn't show, for the MIDI file's THEORY track.
struct StepState {
	deluge::generative::Chord chord; ///< Generator::chord(): scale steps from the tonic, root first.
	int32_t energy = 0;              ///< Generator::effectiveMacro(), macro units.
	int32_t density = 0;
};

/// A played stretch of a Recipe: the events the harness recorded for `bars` bars.
struct Rendering {
	deluge::generative::Recipe recipe;
	/// The Recipe's sound picks: the samples kit rows play, and the presets named in warnings.
	deluge::generative::SoundPicks sounds;
	uint32_t seed = 1;
	uint32_t entropy = 0; ///< The fresh entropy the Recipe's facts were rolled from, for the THEORY track.
	int32_t bars = 0;
	std::vector<deluge::generative::Event> events;
	/// One per decided step, from step 0. May be empty: the THEORY track then has no chords or macros.
	std::vector<StepState> steps;
};

/// Whether an event is heard. Muted note-ons stay in the stream but Perform drops them at the output (spec §12.5), so
/// the render leaves them out of all three files. Their note-offs pass, as in Perform.
constexpr bool heard(const deluge::generative::Event& event) {
	return event.kind != deluge::generative::EventKind::NoteOn || !event.muted;
}

/// Format 1 at 96 PPQN (24 Generator ticks per 16th): a conductor track with the tempo, 4/4 and a marker per Section,
/// then one track per part in roster order. Drums and FX parts play on channel 10 with their lanes on GM drum notes;
/// pitched parts play on their own channel. A last track, THEORY, holds what a theory review reads beside the notes
/// (spec §21.5), as Text events (theoryText() lists them), and no notes.
std::vector<uint8_t> midiFile(const Rendering& rendering);

/// The MIDI note a Drums or FX lane plays on in midiFile(): its GM drum note, or for an FX lane one above the map.
uint8_t laneNote(deluge::generative::Lane lane);

/// The THEORY track's lines with their ticks, in tick order. One word, then `name=value` fields:
/// - at tick 0, `recipe genre= seed= entropy= tempo= swing= energy= density= complexity= motion= phrase=
///   hysteresis=` (macros and the gate hysteresis in OLED points, the tempo in BPM, the phrase in bars);
/// - at tick 0, a `part NAME role= flavour= gate=` per part in roster order (the gate in points), a `lane PART.LANE
///   gate= note=` per lane of a Drums or FX part (note: its MIDI note in the file), and a `rule source=PART[.LANE]
///   kind= target=PART[.LANE] off= strength=` per interplay rule (its effective strength in percent, after Complexity);
/// - `key root= scale= pcs=` at tick 0 and at each KeyScale event: the root's name, the scale and its pitch classes
///   from the root up, comma-separated;
/// - `macros energy= density=` on each bar line where the effective macros differ from the last line's (points);
/// - `chord S,S,S` on each step whose chord differs from the step before's: scale steps from the tonic, root first.
std::vector<std::pair<int32_t, std::string>> theoryText(const Rendering& rendering);

/// A text piano roll, one character per 16th, in blocks of 4 bars: a row per lane of each Drums or FX part and a row
/// per pitch each pitched part sounds, highest first. A hit is 'X' at velocity 100 and up, 'x' from 64, else 'o'; a
/// pitched note held past its 16th shows '=' until its note-off.
std::string pianoRoll(const Rendering& rendering);

/// Where a Recipe's sample paths resolve: each root in order, so pack/ goes before a card-shaped folder.
struct SoundLibrary {
	std::vector<std::string> roots;
};

struct Audio {
	std::vector<uint8_t> wav;
	/// What the render couldn't play as asked: a sample it couldn't find or read (a stand-in hit plays instead), and
	/// sound parameters it doesn't model (ignored). One line each.
	std::vector<std::string> warnings;
};

constexpr int32_t kSampleRate = 44100;
/// The longest WAV the render writes: an hour, 635 MB, well inside the WAV header's 32-bit sizes.
constexpr int64_t kMaxAudioFrames = int64_t{3600} * kSampleRate;

/// Why `bars` bars at `tempo` (hundredths of a BPM) make too long a WAV: past kMaxAudioFrames with the second of tail.
/// Empty when they fit.
std::string audioLengthError(int32_t bars, int32_t tempo);

/// 16-bit stereo at 44.1 kHz: the bars, then a second of tail. Kit rows play their sample once per hit, each hit
/// cutting the row's last; pitched parts play a stand-in subtractive voice per note. Nothing here is random or
/// threaded, so the same input gives the same bytes. Past kMaxAudioFrames it writes nothing (audioLengthError() says
/// why).
Audio renderAudio(const Rendering& rendering, const SoundLibrary& library);

} // namespace harness::render
