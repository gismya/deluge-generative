/*
 * Copyright © 2026 Synthstrom Audible Limited
 *
 * This file is part of The Synthstrom Audible Deluge Firmware.
 *
 * The Synthstrom Audible Deluge Firmware is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

#include "io/midi/test_port.h"

#if ENABLE_SYSEX_TEST

#include "OSLikeStuff/scheduler_api.h"
#include "definitions_cxx.hpp"
#include "drivers/dmac/dmac.h"
#include "extern.h"
#include "generative/core/draw.h"
#include "generative/core/recipe_overrides.h"
#include "generative/firmware/capture_files.h"
#include "generative/firmware/compose_job.h"
#include "generative/firmware/current_recipe.h"
#include "generative/firmware/perform_adapter.h"
#include "generative/firmware/recipe_files.h"
#include "generative/firmware/runner.h"
#include "gui/colour/colour.h"
#include "gui/colour/palette.h"
#include "gui/menu_item/generative/compose_screen.h"
#include "gui/menu_item/generative/genre_rows.h"
#include "gui/ui/audio_recorder.h"
#include "gui/ui/load/load_song_ui.h"
#include "gui/ui/save/save_song_ui.h"
#include "gui/ui/sound_editor.h"
#include "gui/ui/ui.h"
#include "hid/button.h"
#include "hid/buttons.h"
#include "hid/encoders.h"
#include "hid/led/pad_leds.h"
#include "hid/matrix/pad.h"
#include "io/debug/log.h"
#include "io/debug/print.h"
#include "io/midi/midi_device.h"
#include "io/midi/midi_engine.h"
#include "io/midi/midi_follow.h"
#include "io/midi/sysex.h"
#include "model/action/action.h"
#include "model/action/action_logger.h"
#include "model/settings/runtime_feature_settings.h"
#include "model/song/song.h"
#include "playback/playback_handler.h"
#include "processing/engines/audio_engine.h"
#include "storage/storage_manager.h"
#include "util/pack.h"
#include <algorithm>
#include <charconv>
#include <cstdarg>
#include <cstring>
#include <iterator>
#include <new>
#include <strings.h>

namespace TestPort {
namespace {

namespace encoders = deluge::hid::encoders;

// ---- Input queue ----
// Written by the MIDI routine (tail), read by the buttons-and-pads task (head). Both run as scheduler tasks, never
// from an ISR, and each index has one writer, so no locking is needed.

enum class Kind : uint8_t {
	Hid,
	Encoder,
	FastGold,
	Wait,
	SetHold,
	LoadSong,
	SaveSong,    ///< Shares LoadSong's path buffer and its one-at-a-time queue slot
	CheckRecipe, ///< As SaveSong: its name waits in songPath
	ReportInput,
	ReportBootDma,
	ReportSidebar,
	ResetRecipe,
	PickGenre,
	Compose,
	ComposeScreen,
	NextRecipe,     ///< A "next recipe" field: Entry::index a NextField, amount its value
	NextRecipeSound ///< A "next recipe part|lane" pick: as SaveSong, its path waits in songPath
};

/// A "load song", "save song", "check recipe" or "next recipe part|lane" line: its path or name waits in songPath,
/// one at a time.
constexpr bool songFile(Kind kind) {
	return kind == Kind::LoadSong || kind == Kind::SaveSong || kind == Kind::CheckRecipe
	       || kind == Kind::NextRecipeSound;
}

/// The fields of a "next recipe" line (Entry::index of a NextRecipe entry). A Macro's field is kNextMacro plus the
/// Macro. A NextRecipeSound entry's index is the part, or kMaxParts plus the lane.
enum NextField : uint8_t {
	kNextClear,
	kNextGenre,
	kNextSeed,
	kNextEntropy,
	kNextTempo,
	kNextKey,
	kNextScale,
	kNextSwing,
	kNextMacro
};

struct Entry {
	Kind kind;
	/// Hid: PIC byte. Encoder: 0 scroll Y, 1 scroll X, 2 tempo, 3 select, 4 upper gold, 5 lower gold. FastGold: 4 or
	/// 5. SetHold: a row of kHolds, or kAllHolds. PickGenre, Compose: the genre's place in allGenres() (Compose:
	/// kNoGenre for the Current Recipe). Unused for the rest.
	uint8_t index;
	int32_t amount;     ///< Hid: velocity, 0 = release. Encoder, FastGold: signed detents. Wait: ms. SetHold: 1 or 0.
	                    ///< Compose: LENGTH in seconds.
	double arrived = 0; ///< getSystemTime() when the line arrived: input lag counts from here, or the wait before it
};

/// The longest inject line, with its 0: a "next recipe lane" line with a sound pick's longest path.
constexpr size_t kLineLength = 32 + deluge::generative::kMaxSoundPath;

/// The path of the queued "load song" entry, or the name of a "save song" one: one at a time, so a second waits as a
/// full queue would.
char songPath[kLineLength];
bool songLoadQueued = false;
bool songLoading = false; ///< While loadSong() or saveSong() runs: the test port can answer meanwhile, and must not
                          ///< say idle

constexpr uint32_t kQueueSize = 64; // a power of two
// One slot stays free, so putBackHidEvent() never lands on a slot the MIDI routine has refilled since the take.
constexpr uint32_t kQueueCapacity = kQueueSize - 1;
Entry queue[kQueueSize];
uint32_t queueHead = 0;
uint32_t queueTail = 0;

uint32_t queueDepth() {
	return queueTail - queueHead;
}

double resumeAt = 0;                ///< getSystemTime() at which a "wait" entry ends
bool holdingEntry = false;          ///< See entryHeld()
bool holdingRamps = false;          ///< See rampsHeld()
bool holdingStress = false;         ///< See rampsStressed()
bool holdingCardError = false;      ///< See cardErrorHeld()
bool holdingPreload = false;        ///< See preloadHeld()
bool holdingCardRoutine = false;    ///< See cardRoutineHeld()
bool holdingCommitted = false;      ///< See soundCommittedHeld()
bool holdingPresetPreview = false;  ///< See presetPreviewHeld()
bool holdingReactions = false;      ///< See reactionsHeld()
bool holdingUiRelease = false;      ///< See uiReleaseHeld()
bool holdingFree = false;           ///< See freeHeld()
bool holdingQueueFull = false;      ///< See queueFullHeld()
bool holdingLogFull = false;        ///< See logFullHeld()
bool holdingDelaySetups = false;    ///< See delaySetupsHeld()
bool holdingCompose = false;        ///< See composeHeld()
bool holdingComposeAlone = false;   ///< See composeAloneHeld()
bool holdingComposeRoll = false;    ///< See composeRollHeld()
bool holdingComposeReserve = false; ///< See composeReserveHeld()
bool holdingGolden = false;         ///< See goldenHeld()
bool pinningEntropy = false;        ///< See pinnedEntropy()
uint32_t pinnedDraws = 0;           ///< Draws pinnedEntropy() has given since the hold went on

/// "hold midi_setup": MIDI settings for scripts, set in RAM only, so a script on a Deluge doesn't write over the
/// player's own (SETTINGS > MIDI saves to flash). Any cable: MIDI follow channel A on channel 16, its feedback at
/// the HIGH rate, and follow's Track 1, 2 and 3 channels on channels 13, 12 and 11; global commands on channel 15,
/// PLAYBACK_RESTART note 60, PLAY 61, RECORD 62, TAP 63, LOOP 64, LOOP_CONTINUOUS_LAYERING 65, UNDO 66, REDO 67, FILL
/// 68, NEXT_SONG 70, SHIFT 71; TRANSPOSE all of channel 14. MIDI follow maps CCs as stock's defaults do, and its
/// feedback filter is off. "release midi_setup" puts back what was there.
struct MidiSetup {
	LearnedMIDI commands[kNumGlobalMIDICommands];
	LearnedMIDI followA;
	LearnedMIDI followTracks[3];
	MIDIFollowFeedbackChannelType feedbackChannel;
	MIDIFollowFeedbackAutomationMode feedbackRate;
	bool feedbackFilter;
	decltype(MidiFollow::ccToSoundParam) ccToSoundParam;
	decltype(MidiFollow::ccToGlobalParam) ccToGlobalParam;
	decltype(MidiFollow::soundParamToCC) soundParamToCC;
	decltype(MidiFollow::globalParamToCC) globalParamToCC;
};
PLACE_SDRAM_BSS MidiSetup savedMidiSetup;
/// Room for a MidiFollow made only to read stock's default mappings: a card's MIDIFollow.XML may map other CCs.
alignas(MidiFollow) PLACE_SDRAM_BSS uint8_t defaultFollowMemory[sizeof(MidiFollow)];
bool holdingMidiSetup = false;

/// Copies MIDI follow's four mapping tables from `from` to `to`.
template <typename From, typename To>
void copyMappings(const From& from, To& to) {
	to.ccToSoundParam = from.ccToSoundParam;
	to.ccToGlobalParam = from.ccToGlobalParam;
	to.soundParamToCC = from.soundParamToCC;
	to.globalParamToCC = from.globalParamToCC;
}

void holdMidiSetup(bool hold) {
	if (hold == holdingMidiSetup) {
		return;
	}
	holdingMidiSetup = hold;
	LearnedMIDI& followA = midiEngine.midiFollowChannelType[util::to_underlying(MIDIFollowChannelType::A)];
	LearnedMIDI* followTracks = &midiEngine.midiFollowChannelType[util::to_underlying(MIDIFollowChannelType::Track1)];
	if (!hold) {
		std::copy_n(savedMidiSetup.commands, kNumGlobalMIDICommands, midiEngine.globalMIDICommands);
		followA = savedMidiSetup.followA;
		std::copy_n(savedMidiSetup.followTracks, 3, followTracks);
		midiEngine.midiFollowFeedbackChannelType = savedMidiSetup.feedbackChannel;
		midiEngine.midiFollowFeedbackAutomation = savedMidiSetup.feedbackRate;
		midiEngine.midiFollowFeedbackFilter = savedMidiSetup.feedbackFilter;
		copyMappings(savedMidiSetup, midiFollow);
		return;
	}
	std::copy_n(midiEngine.globalMIDICommands, kNumGlobalMIDICommands, savedMidiSetup.commands);
	savedMidiSetup.followA = followA;
	std::copy_n(followTracks, 3, savedMidiSetup.followTracks);
	savedMidiSetup.feedbackChannel = midiEngine.midiFollowFeedbackChannelType;
	savedMidiSetup.feedbackRate = midiEngine.midiFollowFeedbackAutomation;
	savedMidiSetup.feedbackFilter = midiEngine.midiFollowFeedbackFilter;
	copyMappings(midiFollow, savedMidiSetup);
	copyMappings(*new (defaultFollowMemory) MidiFollow(), midiFollow);
	constexpr uint8_t kCommandNotes[] = {60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 255};
	static_assert(std::size(kCommandNotes) == kNumGlobalMIDICommands, "a note (or 255, none) for each command");
	for (int32_t c = 0; c < kNumGlobalMIDICommands; c++) {
		LearnedMIDI& command = midiEngine.globalMIDICommands[c];
		command.clear();
		if (static_cast<GlobalMIDICommand>(c) == GlobalMIDICommand::TRANSPOSE) {
			command.channelOrZone = 13;
		}
		else if (kCommandNotes[c] != 255) {
			command.channelOrZone = 14;
			command.noteOrCC = kCommandNotes[c];
		}
	}
	followA.clear();
	followA.channelOrZone = 15;
	for (int32_t t = 0; t < 3; t++) {
		followTracks[t].clear();
		followTracks[t].channelOrZone = static_cast<uint8_t>(12 - t);
	}
	midiEngine.midiFollowFeedbackChannelType = MIDIFollowFeedbackChannelType::A;
	midiEngine.midiFollowFeedbackAutomation = MIDIFollowFeedbackAutomationMode::HIGH;
	midiEngine.midiFollowFeedbackFilter = false;
}

/// The holds, the one list of them. "hold <name>" sets a row on and "release <name>" sets it off; "release all" sets
/// every row off, as start_state does (to_start_state() in deluge_probe.py), so a script that stopped mid-hold leaves
/// nothing held for the next. A new hold is a row here and its hook, and nothing else
/// needs it added: the probe's HOLDS_BEFORE_RELEASE_ALL is a frozen list for builds older than "release all".
template <uint32_t Ms>
void holdCardStall(bool on) {
	deluge::generative::firmware::cardLog.setStall(on ? Ms : 0);
}

struct Hold {
	const char* name;
	void (*set)(bool on);
};
constexpr Hold kHolds[] = {
    /// Holds Perform entry's job at its first boundary, or lets it go (see entryHeld())
    {"entry", [](bool on) { holdingEntry = on; }},
    /// Stops Perform moving sound controls, or lets it again (see rampsHeld())
    {"ramps", [](bool on) { holdingRamps = on; }},
    /// Re-ramps every sound-control param every step, or stops (see rampsStressed())
    {"stress", [](bool on) { holdingStress = on; }},
    /// Perform's card reads fail as a pulled card's would (see cardErrorHeld())
    {"card_error", [](bool on) { holdingCardError = on; }},
    /// Holds a sound re-roll's or a transition's preload, or a Compose screen card edit (a roll, a LOAD), at its
    /// start, or lets it go (see preloadHeld())
    {"preload", [](bool on) { holdingPreload = on; }},
    /// Injected presses reach their handlers as inside a card wait (sdRoutineLock set, inCardRoutine true), or as
    /// they come (see cardRoutineHeld())
    {"card_routine", [](bool on) { holdingCardRoutine = on; }},
    /// Holds a sound re-roll's preload past its commit, where a cancel no longer stops it (see soundCommittedHeld())
    {"sound_committed", [](bool on) { holdingCommitted = on; }},
    /// Holds a preset preview's build at its start until it is cancelled, or lets it go (see presetPreviewHeld())
    {"preset_preview", [](bool on) { holdingPresetPreview = on; }},
    /// Perform's view stops reacting to a sound swap and a new roster, or reacts again (see reactionsHeld())
    {"reactions", [](bool on) { holdingReactions = on; }},
    /// Perform's pump stops asking the UI to let go of an Output it frees, or asks again (see uiReleaseHeld())
    {"ui_release", [](bool on) { holdingUiRelease = on; }},
    /// Holds Perform's free job at its start, or lets it go (see freeHeld())
    {"free", [](bool on) { holdingFree = on; }},
    /// A waiting sound's cancel or a transition confirm finds the steering queue full, or doesn't (see queueFullHeld())
    {"queue_full", [](bool on) { holdingQueueFull = on; }},
    /// AS IT BEGAN finds no header kept for the playing piece, or reads the log (see logFullHeld())
    {"log_full", [](bool on) { holdingLogFull = on; }},
    /// Perform entry plays the chosen genre with delay setups, or as it is (see delaySetupsHeld())
    {"delay_setups", [](bool on) { holdingDelaySetups = on; }},
    /// Holds a Compose or Capture build after its first bar, or lets it go (see composeHeld())
    {"compose", [](bool on) { holdingCompose = on; }},
    /// A Compose build's estimate fits only alone, or as it is (see composeAloneHeld())
    {"compose_alone", [](bool on) { holdingComposeAlone = on; }},
    /// Holds a Compose build after its genre roll, or lets it go (see composeRollHeld())
    {"compose_roll", [](bool on) { holdingComposeRoll = on; }},
    /// A Compose or Capture build finds the SDRAM reserve reached, or as it is (see composeReserveHeld())
    {"compose_reserve", [](bool on) { holdingComposeReserve = on; }},
    /// A genre roll makes the golden case and a Compose build starts in the Groove, or both as they are (see
    /// goldenHeld())
    {"golden", [](bool on) { holdingGolden = on; }},
    /// The song's fill mode on or off, as the MIDI-learnable FILL command (GlobalMIDICommand::FILL) sets it. There is
    /// no song partway through a song load, and start_state's "release all" can land there.
    {"midi_fill",
     [](bool on) {
	     if (currentSong != nullptr) {
		     currentSong->changeFillMode(on);
	     }
     }},
    /// The card log off: the steering log's bytes are taken and nothing goes to the card (#448's on/off comparison).
    /// Before Perform's entry.
    {"card_log_off", [](bool on) { deluge::generative::firmware::cardLog.setOff(on); }},
    /// Every 2 s the card log's task holds the card for this long without touching it, as a card that stalls (#448)
    {"card_stall_100", holdCardStall<100>},
    {"card_stall_200", holdCardStall<200>},
    {"card_stall_400", holdCardStall<400>},
    {"card_stall_800", holdCardStall<800>},
    /// MIDI follow, feedback and global commands for scripts, RAM only (see MidiSetup)
    {"midi_setup", holdMidiSetup},
    /// Perform's fresh entropy (each Recipe, seed and sound roll) comes from a fixed sequence that starts again here,
    /// so the next entry rolls the same Recipe and part presets on every run from the same card (see pinnedEntropy())
    {"entropy",
     [](bool on) {
	     pinningEntropy = on;
	     pinnedDraws = 0;
     }},
};
constexpr uint8_t kAllHolds = 0xFF; ///< Entry::index of "release all"
constexpr uint8_t kNoGenre = 0xFF;  ///< Entry::index of a "compose" line with no genre: the Current Recipe
static_assert(std::size(kHolds) < kAllHolds);
/// Input lag (spec §12.10's acceptance test), since the last "report input": the longest wait from a press being due
/// (its line arrived and the waits before it ended) to the buttons task taking it, and the longest gap between two
/// gold-knob edges of one turn (the knobs wait out a card read).
double waitEnded = 0;
double maxPressLag = 0;
int32_t presses = 0;
double maxGoldGap = 0;
double lastGoldEdgeAt = 0;
int32_t busyEncoder = -1;
int32_t goldEdgesLeft = 0; ///< Signed edges still to feed to busyEncoder (4 or 5)
double nextGoldEdgeAt = 0;
bool goldFast = false; ///< The turn feeds its edges as fast as the encoder task takes them (an "encoder N D fast" line)

constexpr int32_t kEdgesPerGoldDetent = 4; // one quadrature cycle, as the encoder hardware and delugemu produce
/// Gold-knob edges this far apart keep EncoderAcceleration at 1x (its steady-state multiplier is 0.15 s / interval),
/// so each edge moves the parameter by exactly one step. Faster feeding would multiply the turn by up to 3, unless
/// each edge resets the acceleration first, as a fast turn does.
constexpr double kGoldEdgeInterval = 0.16;
/// Spec §12.10's acceptance test, in seconds: act-at-once inputs and BACK within 20 ms, and a gold-knob turn late by
/// no more than one card wait (6–12 ms a sample in H8; 20 ms allows for a slow one).
constexpr double kMaxPressLag = 0.020;
constexpr double kMaxCardWait = 0.020;

/// Pads and buttons held down by injected events, by PIC byte.
uint32_t injectedHeld[256 / 32];
/// The last injected press: its PIC byte, and AudioEngine::audioSampleTimer when the buttons task took it.
uint8_t lastPressValue = 0;
uint32_t lastPressAt = 0;

encoders::ContinuousEncoder& goldEncoder(int32_t index) {
	return encoders::modEncoderAt(index == 4 ? 1 : 0);
}

bool encoderBusy() {
	if (busyEncoder < 0) {
		return false;
	}
	bool busy = busyEncoder < (int32_t)encoders::kNumFunctionEncoders
	                ? encoders::functionEncoderAt(busyEncoder).pending()
	                : goldEdgesLeft != 0 || goldEncoder(busyEncoder).pending();
	if (!busy) {
		busyEncoder = -1;
	}
	return busy;
}

/// True while an encoder turn, a wait or a song load taken from the queue is still running.
bool entryRunning() {
	return songLoading || encoderBusy() || getSystemTime() < resumeAt;
}

// ---- Parsing ----

constexpr int32_t kMaxTokens = 5;

struct Tokens {
	char* token[kMaxTokens];
	int32_t count;
};

bool isSeparator(char c) {
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

Tokens tokenize(char* line) {
	Tokens t{{}, 0};
	char* p = line;
	while (*p != 0) {
		while (isSeparator(*p)) {
			*p++ = 0;
		}
		if (*p == 0) {
			break;
		}
		if (t.count == kMaxTokens) {
			t.count = kMaxTokens + 1; // too many
			break;
		}
		t.token[t.count++] = p;
		while (*p != 0 && !isSeparator(*p)) {
			p++;
		}
	}
	return t;
}

bool parseInt(const char* s, int32_t min, int32_t max, int32_t* out) {
	if (*s == '+') {
		s++;
	}
	const char* end = s + strlen(s);
	int32_t value;
	auto [last, error] = std::from_chars(s, end, value);
	if (error != std::errc{} || last != end || value < min || value > max) {
		return false;
	}
	*out = value;
	return true;
}

bool parseUint32(const char* s, uint32_t* out) {
	const char* end = s + strlen(s);
	auto [last, error] = std::from_chars(s, end, *out);
	return error == std::errc{} && last == end;
}

/// `start` with the separators around it cut off, or nullptr if nothing is left: a path that is the rest of a line.
const char* restOfLine(char* start) {
	while (isSeparator(*start)) {
		start++;
	}
	char* end = start + strlen(start);
	while (end > start && isSeparator(end[-1])) {
		*--end = 0;
	}
	return end == start ? nullptr : start;
}

/// A "next recipe" line, after its first two words: see parseLine().
bool parseNextRecipe(char* rest, Entry* entry, const char** path) {
	using namespace deluge::generative;
	const bool part = strncmp(rest, "part ", 5) == 0;
	if (part || strncmp(rest, "lane ", 5) == 0) {
		char* number = rest + 5;
		char* space = strchr(number, ' ');
		int32_t slot;
		if (space == nullptr || (*space = 0, !parseInt(number, 0, (part ? kMaxParts : kNumLanes) - 1, &slot))
		    || (*path = restOfLine(space + 1)) == nullptr || strlen(*path) >= kMaxSoundPath) {
			return false;
		}
		*entry = {Kind::NextRecipeSound, static_cast<uint8_t>(part ? slot : kMaxParts + slot), 0};
		return true;
	}
	Tokens t = tokenize(rest);
	const char* field = t.count >= 1 ? t.token[0] : "";
	uint32_t number;
	int32_t value;
	int32_t points;
	if (t.count == 1 && strcmp(field, "clear") == 0) {
		*entry = {Kind::NextRecipe, kNextClear, 0};
	}
	else if (t.count == 2 && strcmp(field, "genre") == 0 && findGenre(t.token[1]) != nullptr) {
		const auto genres = allGenres();
		*entry = {Kind::NextRecipe, kNextGenre,
		          static_cast<int32_t>(std::ranges::find(genres, findGenre(t.token[1])) - genres.begin())};
	}
	else if (t.count == 2 && (strcmp(field, "seed") == 0 || strcmp(field, "entropy") == 0)
	         && parseUint32(t.token[1], &number)) {
		*entry = {Kind::NextRecipe, field[0] == 's' ? kNextSeed : kNextEntropy, static_cast<int32_t>(number)};
	}
	else if (t.count == 3 && strcmp(field, "macro") == 0 && parseInt(t.token[1], 0, kNumMacros - 1, &value)
	         && parseInt(t.token[2], 0, kMacroPoints, &points)) {
		*entry = {Kind::NextRecipe, static_cast<uint8_t>(kNextMacro + value), points};
	}
	else {
		struct Number {
			const char* name;
			NextField field;
			int32_t min, max;
		};
		constexpr Number kNumbers[] = {{"tempo", kNextTempo, kMinTempo, kMaxTempo},
		                               {"key", kNextKey, 0, 11},
		                               {"scale", kNextScale, 0, kNumScales - 1},
		                               {"swing", kNextSwing, -kMaxSwing, kMaxSwing}};
		const Number* row = std::ranges::find_if(kNumbers, [&](const Number& n) { return strcmp(n.name, field) == 0; });
		if (t.count != 2 || row == std::end(kNumbers) || !parseInt(t.token[1], row->min, row->max, &value)) {
			return false;
		}
		*entry = {Kind::NextRecipe, row->field, value};
	}
	return true;
}

bool parseDown(const char* s, bool* down) {
	if (strcmp(s, "down") == 0) {
		*down = true;
		return true;
	}
	if (strcmp(s, "up") == 0) {
		*down = false;
		return true;
	}
	return false;
}

bool buttonFromName(const char* name, uint8_t* value) {
	// The mod buttons by the names on the panel, F1 to F8 (getButtonName() knows only MOD7).
	int32_t f;
	if ((name[0] == 'F' || name[0] == 'f') && parseInt(name + 1, 1, std::size(deluge::hid::button::ZmodButtonX), &f)) {
		*value = deluge::hid::button::fromXY(deluge::hid::button::ZmodButtonX[f - 1],
		                                     deluge::hid::button::ZmodButtonY[f - 1]);
		return true;
	}
	for (int32_t y = 0; y < NUM_BUTTON_ROWS; y++) {
		for (int32_t x = 0; x < NUM_BUTTON_COLS; x++) {
			uint8_t b = deluge::hid::button::fromXY(x, y);
			if (strcasecmp(Buttons::getButtonName(b), name) == 0) {
				*value = b;
				return true;
			}
		}
	}
	return false;
}

/// Line syntax, as delugemu's dz-event plus names and waits:
///   button <x> <y> down|up      button at matrix column x (0-8), row y (0-3)
///   button <NAME> down|up       a name from Buttons::getButtonName(), e.g. SHIFT, SESSION_VIEW, or a mod button's
///                               panel name, F1 to F8
///   pad <x> <y> down|up [vel]   grid pad, x 0-17 (16 and 17 are the sidebar), y 0-7, vel 1-127
///   encoder <i> <detents>       0 scroll Y, 1 scroll X, 2 tempo, 3 select, 4 upper gold, 5 lower gold. A gold
///                               detent is kEdgesPerGoldDetent edges 0.16 s apart, each a one-step edit: 4 edits
///   encoder <4|5> <detents> fast  a gold turn that feeds each detent's edges at once, as fast as the encoder task
///                               takes them: one edit of the same 4 steps per detent, 32 detents in about a second
///   wait <ms>                   holds back the following entries
///   hold <name> | release <name>  sets a hold on or off: <name> is a row of kHolds, which says what each does
///   release all                 sets every hold off
///   reset recipe                the Current Recipe as after boot, see resetRecipe()
///   pick genre <name>           focuses a genre's row in the open GENRE menu by its name (techno-dub), see
///                               pickGenre()
///   load song <path>            loads the song file at <path> on the card (the rest of the line), see loadSong()
///   compose screen              opens the Compose screen over the current UI, see openComposeScreenForTest()
///   compose <seconds> [<genre>] starts a Compose build of that LENGTH (10-3600 s) under the COMPOSING screen, of the
///                               Current Recipe, or of a fresh roll of <genre> (techno-dub), see startCompose()
///   report input                an input_lag event: the longest press lag and gold-knob edge gap since the last one
///   report sidebar              pads_sidebar lit=<n>: how many of the 16 sidebar pads are lit in the frame sent to
///                               the pads (PadLEDs::image)
///   report boot_dma             boot_dma events: the DMA channels that were still running, or had stopped on an
///                               error, when this firmware started (left by the firmware that chainloaded it, see
///                               dmaRecordBootStates() and reportBootDma())
///   save song <name>            saves the current song as SONGS/<name>.XML (the rest of the line), see saveSong()
///   check recipe <name>|*       reads RECIPES/<name>.XML back against the Current Recipe, see checkRecipe()
///   next recipe <field> <value> seam S7: sets one field of the Recipe and seed the next roll takes, see
///                               setNextRecipe(): genre <name>, seed <n>, entropy <n> (0 to 2^32-1), macro <0-3>
///                               <points 0-50>, tempo <hundredths of a BPM>, key <0-11>, scale <a Scale, 0-11>,
///                               swing <-49-49>; part <0-5> <path> and lane <0-14> <path> (the rest of the line)
///   next recipe clear           forgets the fields set
/// For a "load song" or "next recipe part|lane" line, *path points at its path inside `line`; for "save song" and
/// "check recipe", at the name.
bool parseLine(char* line, Entry* entry, const char** path) {
	constexpr char kLoadSong[] = "load song ";
	constexpr char kSaveSong[] = "save song ";
	constexpr char kCheckRecipe[] = "check recipe ";
	constexpr char kNextRecipe[] = "next recipe ";
	if (strncmp(line, kNextRecipe, sizeof(kNextRecipe) - 1) == 0) {
		return parseNextRecipe(line + sizeof(kNextRecipe) - 1, entry, path);
	}
	static_assert(sizeof(kLoadSong) == sizeof(kSaveSong));
	const bool save = strncmp(line, kSaveSong, sizeof(kSaveSong) - 1) == 0;
	const bool check = strncmp(line, kCheckRecipe, sizeof(kCheckRecipe) - 1) == 0;
	if (save || check || strncmp(line, kLoadSong, sizeof(kLoadSong) - 1) == 0) {
		*path = restOfLine(line + (check ? sizeof(kCheckRecipe) : sizeof(kLoadSong)) - 1);
		if (*path == nullptr) {
			return false;
		}
		*entry = {save ? Kind::SaveSong : check ? Kind::CheckRecipe : Kind::LoadSong, 0, 0};
		return true;
	}

	Tokens t = tokenize(line);
	if (t.count < 2 || t.count > kMaxTokens) {
		return false;
	}
	const char* kind = t.token[0];
	int32_t x, y;
	bool down;

	if (strcmp(kind, "button") == 0) {
		uint8_t value;
		if (t.count == 3 && buttonFromName(t.token[1], &value) && parseDown(t.token[2], &down)) {}
		else if (t.count == 4 && parseInt(t.token[1], 0, NUM_BUTTON_COLS - 1, &x)
		         && parseInt(t.token[2], 0, NUM_BUTTON_ROWS - 1, &y) && parseDown(t.token[3], &down)) {
			value = deluge::hid::button::fromXY(x, y);
		}
		else {
			return false;
		}
		*entry = {Kind::Hid, value, down ? USE_DEFAULT_VELOCITY : 0};
		return true;
	}

	if (strcmp(kind, "pad") == 0) {
		int32_t velocity = USE_DEFAULT_VELOCITY;
		if (t.count < 4 || !parseInt(t.token[1], 0, kDisplayWidth + kSideBarWidth - 1, &x)
		    || !parseInt(t.token[2], 0, kDisplayHeight - 1, &y) || !parseDown(t.token[3], &down)
		    || (t.count == 5 && (!down || !parseInt(t.token[4], 1, 127, &velocity)))) {
			return false;
		}
		*entry = {Kind::Hid, Pad(x, y).toChar(), down ? velocity : 0};
		return true;
	}

	if (strcmp(kind, "encoder") == 0) {
		int32_t detents;
		const bool fast = t.count == 4 && strcmp(t.token[3], "fast") == 0;
		if ((t.count != 3 && !fast) || !parseInt(t.token[1], fast ? 4 : 0, 5, &x)
		    || !parseInt(t.token[2], -1000, 1000, &detents)) {
			return false;
		}
		*entry = {fast ? Kind::FastGold : Kind::Encoder, static_cast<uint8_t>(x), detents};
		return true;
	}

	if (strcmp(kind, "hold") == 0 || strcmp(kind, "release") == 0) {
		if (t.count != 2) {
			return false;
		}
		const bool on = kind[0] == 'h';
		if (!on && strcmp(t.token[1], "all") == 0) {
			*entry = {Kind::SetHold, kAllHolds, 0};
			return true;
		}
		for (uint8_t row = 0; row < std::size(kHolds); row++) {
			if (strcmp(t.token[1], kHolds[row].name) == 0) {
				*entry = {Kind::SetHold, row, on ? 1 : 0};
				return true;
			}
		}
		return false;
	}

	if (strcmp(kind, "report") == 0) {
		if (t.count == 2 && strcmp(t.token[1], "input") == 0) {
			*entry = {Kind::ReportInput, 0, 0};
			return true;
		}
		if (t.count == 2 && strcmp(t.token[1], "boot_dma") == 0) {
			*entry = {Kind::ReportBootDma, 0, 0};
			return true;
		}
		if (t.count == 2 && strcmp(t.token[1], "sidebar") == 0) {
			*entry = {Kind::ReportSidebar, 0, 0};
			return true;
		}
		return false;
	}

	if (strcmp(kind, "reset") == 0) {
		if (t.count == 2 && strcmp(t.token[1], "recipe") == 0) {
			*entry = {Kind::ResetRecipe, 0, 0};
			return true;
		}
		return false;
	}

	if (strcmp(kind, "pick") == 0) {
		if (t.count != 3 || strcmp(t.token[1], "genre") != 0) {
			return false;
		}
		const deluge::generative::Genre* genre = deluge::generative::findGenre(t.token[2]);
		if (genre == nullptr) {
			return false;
		}
		const auto genres = deluge::generative::allGenres();
		*entry = {Kind::PickGenre, static_cast<uint8_t>(std::ranges::find(genres, genre) - genres.begin()), 0};
		return true;
	}

	if (strcmp(kind, "compose") == 0 && t.count == 2 && strcmp(t.token[1], "screen") == 0) {
		*entry = {Kind::ComposeScreen, 0, 0};
		return true;
	}

	if (strcmp(kind, "compose") == 0) {
		int32_t seconds;
		if ((t.count != 2 && t.count != 3) || !parseInt(t.token[1], 10, 3600, &seconds)) {
			return false;
		}
		uint8_t index = kNoGenre;
		if (t.count == 3) {
			const deluge::generative::Genre* genre = deluge::generative::findGenre(t.token[2]);
			if (genre == nullptr) {
				return false;
			}
			const auto genres = deluge::generative::allGenres();
			index = static_cast<uint8_t>(std::ranges::find(genres, genre) - genres.begin());
		}
		*entry = {Kind::Compose, index, seconds};
		return true;
	}

	if (strcmp(kind, "wait") == 0) {
		int32_t ms;
		if (t.count != 2 || !parseInt(t.token[1], 0, 60000, &ms)) {
			return false;
		}
		*entry = {Kind::Wait, 0, ms};
		return true;
	}

	return false;
}

// ---- Replies ----

const uint8_t kReplyHeader[] = {SysEx::SYSEX_START,           SysEx::DELUGE_SYSEX_ID_BYTE0,
                                SysEx::DELUGE_SYSEX_ID_BYTE1, SysEx::DELUGE_SYSEX_ID_BYTE2,
                                SysEx::DELUGE_SYSEX_ID_BYTE3, SysEx::SysexCommands::HID};

void sendStatus(MIDICable& cable, uint8_t seq, Status status) {
	uint8_t reply[sizeof(kReplyHeader) + 4];
	memcpy(reply, kReplyHeader, sizeof(kReplyHeader));
	reply[sizeof(kReplyHeader)] = kStatusReply;
	reply[sizeof(kReplyHeader) + 1] = seq;
	reply[sizeof(kReplyHeader) + 2] = static_cast<uint8_t>(status);
	reply[sizeof(kReplyHeader) + 3] = SysEx::SYSEX_END;
	cable.sendSysex(reply, sizeof(reply));
}

/// Checks the optional key after the sequence byte: 00 <key:5>. Returns the offset of the rest of the payload, or -1
/// for a wrong key. `data` is the payload from the HID command byte on, ending in F7.
int32_t checkKey(uint8_t* data, int32_t len) {
	constexpr int32_t kStart = 3;
	if (len < kStart + 7 || data[kStart] != 0x00) {
		return kStart;
	}
	uint32_t received;
	unpack_7bit_to_8bit(reinterpret_cast<uint8_t*>(&received), sizeof(received), data + kStart + 1, 5);
	uint32_t key = runtimeFeatureSettings.get(RuntimeFeatureSettingType::DevSysexAllowed);
	return (key != 0 && received == key) ? kStart + 6 : -1;
}

// ---- Events ----
// Written by event() (tail), sent by sendPendingEvent() (head), both from task context.

// A transition's start step sends about 25 lines within 160 ms, faster than they leave (one a pass of the main
// loop, each after the last has gone). In SDRAM: test builds only.
constexpr uint32_t kEventQueueSize = 64;
static_assert((kEventQueueSize & (kEventQueueSize - 1)) == 0, "a power of two: % stays right as the counters wrap");
constexpr size_t kEventLength = 120;
PLACE_SDRAM_BSS char events[kEventQueueSize][kEventLength];
uint32_t eventHead = 0;
uint32_t eventTail = 0;
uint32_t eventsDropped = 0;
/// Dropped since the last line queued: test_events_dropped n=N marks the place in the log, queued as soon as there is
/// room (sendPendingEvent()), or before the next line that fits.
uint32_t droppedUnmarked = 0;

/// Writes the next free line, its timestamp and then `format` filled from `args`, and moves eventTail on. Its caller
/// has made sure there is room.
void queueLine(const char* format, va_list args) {
	char* line = events[eventTail % kEventQueueSize];
	int32_t prefix = snprintf(line, kEventLength, "%.4f: ", getSystemTime());
	vsnprintf(line + prefix, kEventLength - prefix, format, args);
	eventTail++;
}

void queueLinef(const char* format, ...) {
	va_list args;
	va_start(args, format);
	queueLine(format, args);
	va_end(args);
}

/// Queues test_events_dropped n=N for the lines dropped since the last mark. Its caller has made sure there is room.
void queueDropMark() {
	queueLinef("test_events_dropped n=%lu", static_cast<unsigned long>(droppedUnmarked));
	droppedUnmarked = 0;
}

} // namespace

void injectReceived(MIDICable& cable, uint8_t* data, int32_t len) {
	if (len < 4) {
		return;
	}
	uint8_t seq = data[2] & 0x7F;
	int32_t start = checkKey(data, len);
	if (start < 0) {
		sendStatus(cable, seq, Status::BadKey);
		return;
	}

	char line[kLineLength];
	int32_t lineLength = len - 1 - start; // drop the F7
	if (lineLength <= 0 || lineLength >= (int32_t)sizeof(line)) {
		sendStatus(cable, seq, Status::ParseError);
		return;
	}
	memcpy(line, data + start, lineLength);
	line[lineLength] = 0;

	Entry entry;
	const char* path = nullptr;
	if (!parseLine(line, &entry, &path)) {
		sendStatus(cable, seq, Status::ParseError);
		return;
	}
	if (queueDepth() >= kQueueCapacity || (songFile(entry.kind) && songLoadQueued)) {
		sendStatus(cable, seq, Status::QueueFull);
		return;
	}
	if (songFile(entry.kind)) {
		strcpy(songPath, path); // both are kLineLength
		songLoadQueued = true;
	}
	entry.arrived = getSystemTime();
	queue[queueTail % kQueueSize] = entry;
	queueTail++;
	sendStatus(cable, seq, Status::Queued);
}

namespace {

/// Actions in one direction of the undo log: BEFORE = undoable, AFTER = redoable.
uint32_t actionLogLength(TimeType time) {
	uint32_t count = 0;
	for (Action* action = actionLogger.firstAction[time]; action != nullptr; action = action->nextAction) {
		count++;
	}
	return count;
}

/// "report boot_dma": boot_dma running=<n> mask=<channels> (running or errored), then boot_dma_channel ch=... for each
/// of them. A channel counts if it was enabled or mid-transfer (EN, TACT), or had stopped on an error (ER, DER): a load
/// whose channel stopped on an error doesn't pass. END alone (a channel that finished) doesn't count.
void reportBootDma() {
	// Every line fits in the event queue, also with a few others already queued
	static_assert(kEventQueueSize >= 2 * (1 + NUM_DMA_CHANNELS));
	constexpr uint32_t kCounted = DMAC0_CHSTAT_n_EN | DMAC0_CHSTAT_n_TACT | DMAC0_CHSTAT_n_ER | DMAC0_CHSTAT_n_DER;
	uint32_t mask = 0;
	int32_t running = 0;
	for (int32_t ch = 0; ch < NUM_DMA_CHANNELS; ch++) {
		if (dmaBootStates[ch].chstat & kCounted) {
			mask |= 1u << ch;
			running++;
		}
	}
	event("boot_dma running=%d mask=0x%04x", static_cast<int>(running), static_cast<unsigned>(mask));
	for (int32_t ch = 0; ch < NUM_DMA_CHANNELS; ch++) {
		const DmaBootState& b = dmaBootStates[ch];
		if (mask & (1u << ch)) {
			event("boot_dma_channel ch=%d chstat=0x%08x crla=0x%08x crsa=0x%08x crda=0x%08x", static_cast<int>(ch),
			      static_cast<unsigned>(b.chstat), static_cast<unsigned>(b.crla), static_cast<unsigned>(b.crsa),
			      static_cast<unsigned>(b.crda));
		}
	}
}

const char* recordingName() {
	switch (playbackHandler.recording) {
	case RecordingMode::OFF:
		return "off";
	case RecordingMode::NORMAL:
		return "normal";
	case RecordingMode::ARRANGEMENT:
		return "arrangement";
	}
	return "unknown";
}

} // namespace

void stateReceived(MIDICable& cable, uint8_t* data, int32_t len) {
	if (len < 4) {
		return;
	}
	uint8_t seq = data[2] & 0x7F;
	if (checkKey(data, len) < 0) {
		sendStatus(cable, seq, Status::BadKey);
		return;
	}

	UI* ui = getCurrentUI();
	RootUI* root = getRootUI();
	uint8_t reply[sizeof(kReplyHeader) + 2 + 480];
	size_t pos = sizeof(kReplyHeader);
	memcpy(reply, kReplyHeader, pos);
	reply[pos++] = kStateReply;
	reply[pos++] = seq;
	size_t room = sizeof(reply) - pos; // the F7 goes where snprintf puts its 0
	int32_t jsonLength = snprintf(
	    reinterpret_cast<char*>(reply + pos), room,
	    "{\"ui\":\"%s\",\"root_ui\":\"%s\",\"ui_mode\":%u,\"playing\":%s,\"bpm\":%.2f,\"swing\":%d,"
	    "\"voices\":%d,\"cpu_direness\":%d,\"sd_routine_lock\":%s,\"accessing_card\":%u,"
	    "\"inject_queue\":%u,\"event_queue\":%u,\"events_dropped\":%u,\"sample_timer\":%u,\"recording\":\"%s\","
	    "\"resampling\":%s,"
	    "\"undo_log\":%u,\"redo_log\":%u,\"reversion\":%s}",
	    ui ? ui->getUIName() : "none", root ? root->getUIName() : "none", static_cast<unsigned>(currentUIMode),
	    playbackHandler.isEitherClockActive() ? "true" : "false",
	    currentSong ? playbackHandler.calculateBPMForDisplay() : 0.0f, currentSong ? currentSong->swingAmount : 0,
	    static_cast<int>(AudioEngine::getNumVoices()), static_cast<int>(AudioEngine::cpuDireness),
	    sdRoutineLock ? "true" : "false", static_cast<unsigned>(currentlyAccessingCard),
	    static_cast<unsigned>(queueDepth() + (entryRunning() ? 1 : 0)), static_cast<unsigned>(eventTail - eventHead),
	    static_cast<unsigned>(eventsDropped), static_cast<unsigned>(AudioEngine::audioSampleTimer), recordingName(),
	    audioRecorder.isCurrentlyResampling() ? "true" : "false", static_cast<unsigned>(actionLogLength(BEFORE)),
	    static_cast<unsigned>(actionLogLength(AFTER)), actionLogger.allowedToDoReversion() ? "true" : "false");
	pos += std::min<int32_t>(jsonLength, room - 1);
	reply[pos++] = SysEx::SYSEX_END;
	cable.sendSysex(reply, pos);
}

namespace {

/// "load song PATH": loads the song file at PATH the way the boot song is loaded (setupStartupSong()): the current
/// song takes the path, the song browser opens on that file, and performLoad() swaps the song in. Sends
/// song_loaded with the time it took, or song_load_failed with why: busy (something is open over the root UI, or
/// Perform is, whose song performAdapter still uses; nothing changed), missing (no such file, nothing changed) or
/// load (the browser's load failed and left another song).
void loadSong(const char* path) {
	if (rootUIIsGenerativePerform() || getCurrentUI() != getRootUI()) {
		event("song_load_failed path=%s why=busy", path);
		return;
	}
	if (!StorageManager::fileExists(path)) {
		event("song_load_failed path=%s why=missing", path);
		return;
	}
	// A loaded song is named after its file, without the extension (Browser::setEnteredTextFromCurrentFilename()).
	const char* slash = strrchr(path, '/');
	const char* file = slash != nullptr ? slash + 1 : path;
	const char* dot = strrchr(file, '.');
	size_t stemLength = dot != nullptr ? dot - file : strlen(file);

	double start = getSystemTime();
	songLoading = true;
	currentSong->setSongFullPath(path);
	if (openUI(&loadSongUI)) {
		loadSongUI.performLoad();
	}
	songLoading = false;
	const char* name = currentSong->name.get();
	if (strlen(name) == stemLength && strncasecmp(name, file, stemLength) == 0) {
		long outputs = 0;
		long instances = 0;
		for (Output* out = currentSong->firstOutput; out != nullptr; out = out->next) {
			outputs++;
			instances += out->clipInstances.getNumElements();
		}
		event("song_loaded path=%s ms=%d outputs=%ld instances=%ld", path,
		      static_cast<int>((getSystemTime() - start) * 1000), outputs, instances);
	}
	else {
		event("song_load_failed path=%s why=load", path);
	}
}

/// Seam S7's Recipe and seed for the next roll, while nextRecipeArmed. Zero-filled at boot: the first field set clears
/// it to its defaults.
PLACE_SDRAM_BSS deluge::generative::NextRecipe nextRecipeValue;
bool nextRecipeArmed = false;

deluge::generative::NextRecipe& armNextRecipe() {
	if (!nextRecipeArmed) {
		nextRecipeValue.clear();
		nextRecipeArmed = true;
	}
	return nextRecipeValue;
}

/// "next recipe <field> <value>": sets the field in the Recipe and seed the next roll takes (nextRecipe()); "next
/// recipe clear" forgets them all, and so does "reset recipe". The roll that takes them sends gen_next_recipe taken.
void setNextRecipe(uint8_t field, int32_t value) {
	using namespace deluge::generative;
	if (field == kNextClear) {
		nextRecipeArmed = false;
		return;
	}
	NextRecipe& next = armNextRecipe();
	RecipeOverrides& overrides = next.overrides;
	switch (field) {
	case kNextGenre:
		overrides.genre = allGenres()[value]->name;
		break;
	case kNextSeed:
		overrides.seed = static_cast<uint32_t>(value);
		next.seedSet = true;
		break;
	case kNextEntropy:
		overrides.entropy = static_cast<uint32_t>(value);
		next.entropySet = true;
		break;
	case kNextTempo:
		overrides.tempo = value;
		break;
	case kNextKey:
		overrides.keyRoot = static_cast<int8_t>(value);
		break;
	case kNextScale:
		overrides.scale = static_cast<int8_t>(value);
		break;
	case kNextSwing:
		overrides.swing = static_cast<int8_t>(value);
		break;
	default:
		overrides.macroPoints[field - kNextMacro] = static_cast<int8_t>(value);
	}
}

/// "next recipe part|lane <n> <path>": the sound pick the next roll takes for that part or lane.
void setNextRecipeSound(uint8_t slot, const char* path) {
	using namespace deluge::generative;
	SoundPicks& sounds = armNextRecipe().sounds;
	SoundPath& pick = slot < kMaxParts ? sounds.part[slot] : sounds.lane[slot - kMaxParts];
	pick.set(path); // parseNextRecipe() refused a path too long for it
}

/// "reset recipe": puts the Current Recipe back as after boot (forgetCurrentRecipe()), so the next Perform entry's
/// GENRE ▸ opens on the default genre (techno-acid) and rolls a fresh Recipe, seed and sound picks for it, whatever
/// genre the last script left playing. start_state sends it once it has left Perform. Sends gen_recipe_reset done,
/// or gen_recipe_reset refused with why: perform (Perform is open or still holds its generator, which plays the
/// Recipe; nothing changed) or busy (a runner job, which may still commit a Recipe, hasn't ended; nothing changed).
void resetRecipe() {
	using namespace deluge::generative::firmware;
	nextRecipeArmed = false;
	if (rootUIIsGenerativePerform() || performAdapter.active()) {
		event("gen_recipe_reset refused why=perform");
		return;
	}
	if (runner.busy()) {
		event("gen_recipe_reset refused why=busy");
		return;
	}
	forgetCurrentRecipe();
	event("gen_recipe_reset done");
}

/// "pick genre <name>": focuses the genre's row in the GENRE ▸ menu or the parent's submenu the sound editor shows
/// (pickGenreForTest()), so a script picks a genre by name, whatever rows other genres add. In GENRE ▸ a subgenre's
/// row is its parent's: open it with SELECT, then pick the genre again. Sends gen_genre_pick <name> row=<label now
/// focused> from=<label focused before: where the menu opened, right after a SELECT>, or gen_genre_pick <name>
/// refused (no genre menu is open, or the genre isn't in it; nothing moved).
void pickGenre(const deluge::generative::Genre& genre) {
	MenuItem* current = getCurrentUI() == &soundEditor ? soundEditor.getCurrentMenuItem() : nullptr;
	std::string_view row;
	std::string_view from;
	if (!deluge::gui::menu_item::generative::pickGenreForTest(current, genre, row, from)) {
		event("gen_genre_pick %s refused", genre.name);
		return;
	}
	event("gen_genre_pick %s row=%.*s from=%.*s", genre.name, static_cast<int>(row.size()), row.data(),
	      static_cast<int>(from.size()), from.data());
}

/// "save song <name>": saves the current song as SONGS/<name>.XML through the stock save UI, overwriting, as SAVE twice
/// with that name would, then sends song_saved name=... ms=..., or song_save_failed name=... why=busy|save (busy: in
/// Perform, or something open over the root UI). For songs the scripts build (a Compose song for H19).
void saveSong(const char* name) {
	if (rootUIIsGenerativePerform() || getCurrentUI() != getRootUI()) {
		event("song_save_failed name=%s why=busy", name);
		return;
	}
	double start = getSystemTime();
	songLoading = true;
	currentSong->name.set(name);
	currentSong->dirPath.set("SONGS");
	const bool saved = openUI(&saveSongUI) && saveSongUI.performSave(true);
	if (getCurrentUI() == &saveSongUI) {
		saveSongUI.close(); // performSave() closes it only on success
	}
	songLoading = false;
	if (saved) {
		// Where the save UI wrote it: it names the song after the file.
		event("song_saved name=%s ms=%d path=%s/%s.XML", name, static_cast<int>((getSystemTime() - start) * 1000),
		      currentSong->dirPath.get(), currentSong->name.get());
	}
	else {
		event("song_save_failed name=%s why=save", name);
	}
}

/// "check recipe <name>": reads RECIPES/<name>.XML as a Recipe LOAD would (spec §16.1) and compares it with the Current
/// Recipe: gen_recipe_check name=... read=ok|other_version|unknown_genre|unreadable error=<card error> match=1|0
/// recipe=1|0 seed=1|0 takes=1|0 sounds=1|0, each 1 when the file's equals the Current Recipe's. A saved Recipe
/// reloads to the same Recipe, seed and takes (#138's Done-when). The name `*` is the one the Current Recipe remembers.
void checkRecipe(const char* name) {
	using namespace deluge::generative;
	if (strcmp(name, "*") == 0) {
		name = firmware::currentRecipe.name.text;
	}
	static PLACE_SDRAM_BSS RecipeFile file; // a set of sound picks: not on the stack
	char path[perform::kMaxRecipeNameChars + 16];
	snprintf(path, sizeof(path), "%s/%s.XML", firmware::kRecipeFolder, name);
	Error error = Error::NONE;
	const RecipeFileRead read = firmware::readRecipeFile(path, file, error);
	constexpr const char* kReads[] = {"ok", "other_version", "unknown_genre", "unreadable"};
	const firmware::CurrentRecipe& current = firmware::currentRecipe;
	const bool loaded = read == RecipeFileRead::Ok || read == RecipeFileRead::OtherVersion;
	const bool recipe = loaded && file.recipe == current.recipe;
	const bool seed = loaded && file.seed == current.seed;
	const bool takes = loaded && file.takes == current.takes;
	bool sounds = loaded;
	for (int32_t p = 0; sounds && p < kMaxParts; p++) {
		sounds = strcmp(file.sounds.part[p].c_str(), current.sounds.part[p].c_str()) == 0;
	}
	for (int32_t l = 0; sounds && l < kNumLanes; l++) {
		sounds = strcmp(file.sounds.lane[l].c_str(), current.sounds.lane[l].c_str()) == 0;
	}
	event("gen_recipe_check name=%s read=%s error=%d match=%d recipe=%d seed=%d takes=%d sounds=%d", name,
	      kReads[static_cast<int32_t>(read)], static_cast<int>(error), recipe && seed && takes && sounds ? 1 : 0,
	      recipe ? 1 : 0, seed ? 1 : 0, takes ? 1 : 0, sounds ? 1 : 0);
}

/// "compose <seconds> [<genre>]": a Compose build as BUILD starts it (spec §14.5), without the screen and DISCARD
/// SONG?, and of any length or a fresh genre roll, for scripts that test the build itself: beginCompose() opens the
/// COMPOSING screen over the view and starts the job; gen_compose refused why=perform|busy|recipe says why not (Perform
/// is active, the runner is busy, no Current Recipe exists). The job's own events follow.
void startCompose(const deluge::generative::Genre* genre, int32_t seconds) {
	using namespace deluge::generative::firmware;
	const ComposeStart start = beginCompose(genre, seconds, nullptr);
	if (start != ComposeStart::Started) {
		event("gen_compose refused why=%s", start == ComposeStart::PerformActive ? "perform"
		                                    : start == ComposeStart::RunnerBusy  ? "busy"
		                                                                         : "recipe");
	}
}

/// "compose screen": opens the Compose screen (spec §14.4) over the current UI, with no Recipe too, which the
/// Generative menu's COMPOSE FROM never shows (its rows make one first). The screen sends gen_compose_screen opened
/// as it opens; gen_compose_screen refused why=perform says it can't (Perform is the root UI).
void openComposeScreenForTest() {
	if (!deluge::gui::menu_item::generative::openComposeScreen()) {
		event("gen_compose_screen refused why=perform");
	}
}

} // namespace

std::optional<HidEvent> takeHidEvent() {
	while (queueHead != queueTail) {
		if (entryRunning()) {
			return std::nullopt;
		}
		Entry entry = queue[queueHead % kQueueSize];
		if (songFile(entry.kind) && sdRoutineLock) {
			return std::nullopt; // the browser loads only outside the card routine
		}
		queueHead++;
		switch (entry.kind) {
		case Kind::Hid: {
			const double lag = getSystemTime() - std::max(entry.arrived, waitEnded);
			maxPressLag = std::max(maxPressLag, lag);
			presses += entry.amount != 0 ? 1 : 0;
			if (lag > kMaxPressLag) {
				event("input_late value=%u on=%d lag_ms=%d sd_lock=%d", static_cast<unsigned>(entry.index),
				      entry.amount != 0 ? 1 : 0, static_cast<int>(lag * 1000 + 0.5), sdRoutineLock ? 1 : 0);
			}
			uint32_t bit = 1u << (entry.index % 32);
			if (entry.amount != 0) {
				injectedHeld[entry.index / 32] |= bit;
				lastPressValue = entry.index;
				lastPressAt = AudioEngine::audioSampleTimer;
			}
			else {
				injectedHeld[entry.index / 32] &= ~bit;
			}
			return HidEvent{entry.index, entry.amount};
		}

		case Kind::Wait:
			resumeAt = getSystemTime() + entry.amount / 1000.0;
			waitEnded = resumeAt;
			break;

		case Kind::ReportBootDma:
			reportBootDma();
			break;

		case Kind::ReportSidebar: {
			// What the sidebar pads show: PadLEDs::image is the frame last sent to them.
			int32_t lit = 0;
			for (int32_t y = 0; y < kDisplayHeight; y++) {
				for (int32_t x = kDisplayWidth; x < kDisplayWidth + kSideBarWidth; x++) {
					lit += PadLEDs::image[y][x] != deluge::gui::colours::black ? 1 : 0;
				}
			}
			event("pads_sidebar lit=%d", static_cast<int>(lit));
			break;
		}

		case Kind::ReportInput: {
			// Spec §12.10's bounds: a press within 20 ms, and a knob edge late by no more than a card wait.
			const bool ok = maxPressLag <= kMaxPressLag && maxGoldGap <= kGoldEdgeInterval + kMaxCardWait;
			event("input_lag ok=%s max_ms=%d presses=%d gold_gap_ms=%d", ok ? "yes" : "no",
			      static_cast<int>(maxPressLag * 1000 + 0.5), static_cast<int>(presses),
			      static_cast<int>(maxGoldGap * 1000 + 0.5));
			maxPressLag = 0;
			presses = 0;
			maxGoldGap = 0;
			break;
		}

		case Kind::SetHold:
			if (entry.index == kAllHolds) {
				for (const Hold& hold : kHolds) {
					hold.set(false);
				}
				break;
			}
			kHolds[entry.index].set(entry.amount != 0);
			break;

		case Kind::ResetRecipe:
			resetRecipe();
			break;

		case Kind::PickGenre:
			pickGenre(*deluge::generative::allGenres()[entry.index]);
			break;

		case Kind::ComposeScreen:
			openComposeScreenForTest();
			break;

		case Kind::Compose:
			startCompose(entry.index == kNoGenre ? nullptr : deluge::generative::allGenres()[entry.index],
			             entry.amount);
			break;

		case Kind::LoadSong:
			loadSong(songPath);
			songLoadQueued = false;
			return std::nullopt; // one load per call: let the main loop catch up after it

		case Kind::SaveSong:
			saveSong(songPath);
			songLoadQueued = false;
			return std::nullopt; // as a load: let the main loop catch up after it

		case Kind::CheckRecipe:
			checkRecipe(songPath);
			songLoadQueued = false;
			return std::nullopt;

		case Kind::NextRecipe:
			setNextRecipe(entry.index, entry.amount);
			break;

		case Kind::NextRecipeSound:
			setNextRecipeSound(entry.index, songPath);
			songLoadQueued = false;
			break;

		case Kind::Encoder:
		case Kind::FastGold:
			busyEncoder = entry.index;
			if (entry.index < encoders::kNumFunctionEncoders) {
				encoders::functionEncoderAt(entry.index).restore(entry.amount);
			}
			else {
				goldEdgesLeft = entry.amount * kEdgesPerGoldDetent;
				goldFast = entry.kind == Kind::FastGold;
				lastGoldEdgeAt = 0; // a new turn: its first edge has no gap
			}
			unblockTask(encoders::EncoderTaskID);
			break;
		}
	}
	return std::nullopt;
}

void putBackHidEvent() {
	event("input_deferred value=%u", static_cast<unsigned>(queue[(queueHead - 1) % kQueueSize].index));
	queueHead--;
}

bool holdingInjectedPresses() {
	for (uint32_t word : injectedHeld) {
		if (word != 0) {
			return true;
		}
	}
	return false;
}

void feedEncoderEdges() {
	if (goldEdgesLeft == 0 || busyEncoder < (int32_t)encoders::kNumFunctionEncoders) {
		return;
	}
	auto& encoder = goldEncoder(busyEncoder);
	// One edge per take, slow enough to stay clear of the acceleration: one edit per edge. A fast turn instead
	// feeds a whole detent per take, as a quick hardware turn batches its edges, with the acceleration reset so the
	// batch reads as a turn's first at 1x: one edit of kEdgesPerGoldDetent steps per detent.
	double now = getSystemTime();
	if (!encoder.pending() && (goldFast || now >= nextGoldEdgeAt)) {
		const int8_t edge =
		    static_cast<int8_t>(goldFast ? std::clamp(goldEdgesLeft, -kEdgesPerGoldDetent, kEdgesPerGoldDetent)
		                                 : (goldEdgesLeft > 0 ? 1 : -1));
		if (goldFast) {
			encoder.resetAcceleration();
		}
		if (lastGoldEdgeAt > 0) {
			maxGoldGap = std::max(maxGoldGap, now - lastGoldEdgeAt);
		}
		lastGoldEdgeAt = now;
		encoder.apply_edges(edge);
		goldEdgesLeft -= edge;
		nextGoldEdgeAt = now + kGoldEdgeInterval;
	}
}

bool entryHeld() {
	return holdingEntry;
}

const deluge::generative::NextRecipe* nextRecipe() {
	return nextRecipeArmed ? &nextRecipeValue : nullptr;
}

void nextRecipeTaken() {
	nextRecipeArmed = false;
	event("gen_next_recipe taken");
}

bool pinnedEntropy(uint32_t* out) {
	if (!pinningEntropy) {
		return false;
	}
	*out = deluge::generative::detail::mix(0x7e57e000U + ++pinnedDraws);
	return true;
}

int32_t injectedHoldMs(uint8_t value) {
	if (value != lastPressValue || (injectedHeld[value / 32] & (1u << (value % 32))) == 0) {
		return -1;
	}
	return samplesToMs(AudioEngine::audioSampleTimer - lastPressAt);
}

int32_t samplesToMs(uint32_t samples) {
	return static_cast<int32_t>(static_cast<uint64_t>(samples) * 1000 / kSampleRate);
}

bool rampsHeld() {
	return holdingRamps;
}

bool rampsStressed() {
	return holdingStress;
}

bool cardErrorHeld() {
	return holdingCardError;
}

bool composeHeld() {
	return holdingCompose;
}

bool composeAloneHeld() {
	return holdingComposeAlone;
}

bool composeRollHeld() {
	return holdingComposeRoll;
}

bool composeReserveHeld() {
	return holdingComposeReserve;
}

bool goldenHeld() {
	return holdingGolden;
}

bool preloadHeld() {
	return holdingPreload;
}

bool presetPreviewHeld() {
	return holdingPresetPreview;
}

bool cardRoutineHeld() {
	return holdingCardRoutine;
}

bool soundCommittedHeld() {
	return holdingCommitted;
}

void releaseSoundCommitted() {
	holdingCommitted = false;
}

bool reactionsHeld() {
	return holdingReactions;
}

bool uiReleaseHeld() {
	return holdingUiRelease;
}

bool freeHeld() {
	return holdingFree;
}

bool queueFullHeld() {
	return holdingQueueFull;
}

bool logFullHeld() {
	return holdingLogFull;
}

bool delaySetupsHeld() {
	return holdingDelaySetups;
}

void releasePreload() {
	holdingPreload = false;
}

void releaseEntry() {
	holdingEntry = false;
}

void event(const char* format, ...) {
	if (Debug::midiDebugCable == nullptr) {
		return;
	}
	// Room for this line, and for the mark before it when lines were dropped.
	if (eventTail - eventHead + (droppedUnmarked > 0 ? 1 : 0) >= kEventQueueSize) {
		eventsDropped++;
		droppedUnmarked++;
		return;
	}
	if (droppedUnmarked > 0) {
		queueDropMark();
	}
	va_list args;
	va_start(args, format);
	queueLine(format, args);
	va_end(args);
}

void sendPendingEvent() {
	// No cable: logging is off, so drop what's queued and any drop count, or the next session starts with a stale mark.
	if (Debug::midiDebugCable == nullptr) {
		eventHead = eventTail;
		droppedUnmarked = 0;
		return;
	}
	// Drops at the end of a burst, with no line after them, still get their mark once a line has left.
	if (droppedUnmarked > 0 && eventTail - eventHead < kEventQueueSize) {
		queueDropMark();
	}
	if (eventHead == eventTail) {
		return;
	}
	// Bursts overflow the USB and DIN sysex buffers, which drop what doesn't fit. So one line at a time, each after
	// the previous one has left.
	if (midiEngine.anythingInOutputBuffer()) {
		midiEngine.flushMIDI();
		return;
	}
	Debug::sysexDebugPrint(*Debug::midiDebugCable, events[eventHead % kEventQueueSize], true, kEventCategory);
	midiEngine.flushMIDI();
	eventHead++;
}

} // namespace TestPort

#endif // ENABLE_SYSEX_TEST
