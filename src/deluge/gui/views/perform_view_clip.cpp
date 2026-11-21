/*
 * Copyright © 2026 Lars Johansson
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

// PerformView's CLIP page (spec §13.5): the part's pads and faders, SOUND EDITOR and the sound editor over Perform
// (§13.7), NEW SOUND and the preset browser, FREEZE and takes.

#include "gui/views/perform_view_internal.h"

using deluge::generative::Generator;
using deluge::generative::SoundControl;
using deluge::generative::SoundPath;
using deluge::generative::firmware::currentRecipe;
using deluge::generative::firmware::PartOutput;
using deluge::generative::firmware::performAdapter;
using deluge::generative::firmware::pressShowingWork;
using deluge::generative::firmware::ReleaseAnswer;
using deluge::generative::firmware::soundControls;
using deluge::generative::firmware::SoundSwaps;
using deluge::generative::firmware::soundSwaps;
using deluge::generative::firmware::workingAnimationShown;
using deluge::generative::perform::PressOutcome;
using deluge::gui::refusedWithPopup;
using namespace perform_view_shared;

namespace {

/// The CLIP page's FREEZE pad, bright while the part is frozen, and NEW SOUND (the control-layout prototype's).
constexpr RGB kFrozenColour = RGB(0, 200, 255);

constexpr RGB kNewSoundColour = RGB(120, 60, 0);

/// The CLIP page's SOUND EDITOR pad (tuning).
constexpr RGB kSoundEditorColour = RGB(90, 40, 150);

} // namespace

InstrumentClip* PerformView::partClip(int32_t part) {
	if (part < 0 || !performAdapter.active() || part >= performAdapter.steering().rosterParts()) {
		return nullptr;
	}
	return performAdapter.output().roster.parts[part].clip;
}

ActionResult PerformView::openSoundEditorAtPad(int32_t part, int32_t x, int32_t y) {
	return openSoundEditor(
	    part, [x, y](InstrumentClip* /*clip*/) { return soundEditor.potentialShortcutPadAction(x, y, true); });
}

ActionResult PerformView::openSoundEditorRootMenu(int32_t part) {
	return openSoundEditor(part, [](InstrumentClip* clip) {
		if (soundEditor.setup(clip)) {
			soundEditor.enterOrUpdateSoundEditor(true);
		}
		return ActionResult::DEALT_WITH;
	});
}

template <typename Open>
ActionResult PerformView::openSoundEditor(int32_t part, Open openEditor) {
	if (sdRoutineLock) {
		return ActionResult::REMIND_ME_OUTSIDE_CARD_ROUTINE;
	}
	InstrumentClip* clip = partClip(part);
	if (part >= 0 && clip == nullptr) {
		return ActionResult::DEALT_WITH; // a part not on the roster, or not built: nothing to open (-1 is the song FX)
	}
	if (clip != nullptr) {
		affectWholeKit(*clip);
	}
	// The editor anywhere in the UI stack: a shortcut can open a UI over it (OSC 1's file browser opens the sample
	// browser), and the current Clip must stay its part's under that UI.
	const bool wasOpen = isUIOpen(&soundEditor);
	Clip* const wasClip = currentSong->getCurrentClip();
	ModControllableAudio* const wasEditing = soundEditor.currentModControllable;
	MenuItem* const wasItem = wasOpen ? soundEditor.getCurrentMenuItem() : nullptr;
	const bool wasKitFX = soundEditor.setupKitGlobalFXMenu;
	// The stock editor reads the song's current Clip throughout, and Perform has none: while the editor is open on a
	// part, that part's clip is current, which F6 reads as a clip context. focusRegained() clears it.
	currentSong->setCurrentClip(clip);
	soundEditor.setupKitGlobalFXMenu = SoundEditor::shouldEditKitAffectEntire();
	const ActionResult result = openEditor(clip);
	const bool open = isUIOpen(&soundEditor);
	const bool targetMoved = !wasOpen || soundEditor.currentModControllable != wasEditing;
	if (!open || !targetMoved) {
		// It didn't open, or it's still on the Sound it was on (the param doesn't apply to the new target, or the pad
		// has none): so is the current Clip.
		currentSong->setCurrentClip(open ? wasClip : nullptr);
		soundEditor.setupKitGlobalFXMenu = wasKitFX;
	}
	if (!open || (!targetMoved && soundEditor.getCurrentMenuItem() == wasItem)) {
		// Nothing opened: a Perform menu under it stays a menu, closed by a pad press (spec §13.6).
		TEST_EVENT("gen_editor %s", !open ? "none" : wasClip != nullptr ? "stayed part" : "stayed song");
		return result;
	}
	editorOpen_ = true;
#if ENABLE_SYSEX_TEST
	uint32_t cols = 0;
	uint32_t rows = 0;
	const bool greyout = soundEditor.getGreyoutColsAndRows(&cols, &rows);
	TEST_EVENT("gen_editor open part=%ld kit=%d greyout=%d", static_cast<long>(clip != nullptr ? part : -1),
	           soundEditor.editingKitAffectEntire() ? 1 : 0, greyout ? 1 : 0);
#endif
	return result;
}

void PerformView::closeEditorOnRetiredPart() {
	Clip* const clip = currentSong->getCurrentClip(); // set only while the editor is open on a part
	if (clip == nullptr || !isUIOpen(&soundEditor)) {
		return;
	}
	if (performAdapter.active()) {
		const auto& roster = performAdapter.output().roster;
		for (int32_t p = 0; p < performAdapter.steering().rosterParts(); p++) {
			if (roster.parts[p].clip == clip) {
				return;
			}
		}
	}
	if (getCurrentUI() != &soundEditor) {
		// A UI over the editor (a browser) may still use the Sound: releaseOutput() holds its Output until the editor
		// is on top again and closes, every frame till then.
		return;
	}
	closeRetiredEditor();
}

void PerformView::closeRetiredEditor() {
	TEST_EVENT("gen_editor retired");
	soundEditor.exitCompletely();
}

ReleaseAnswer PerformView::releaseOutput(const PartOutput& output) {
	// The gold knobs, and what follows them (MIDI follow, the knob popup, the rings): back onto the roster.
	if (deluge::generative::firmware::knobsOn(output)) {
		bindKnobs();
		refreshFaders();
		if (deluge::generative::firmware::knobsOn(output)) {
			return ReleaseAnswer::Held;
		}
	}
	// A sound editor open on it: the song's current Clip is its clip (openSoundEditor()), and the editor and any UI
	// over it (sample browser, audio recorder, sample marker editor) read the Sound through that or the editor's own
	// pointers. On top, it closes, which clears the current Clip; under another UI it can't, so the Output waits.
	if (output.clip != nullptr && currentSong->getCurrentClip() == output.clip) {
		if (getCurrentUI() != &soundEditor) {
			return ReleaseAnswer::Held;
		}
		closeRetiredEditor();
		if (currentSong->getCurrentClip() == output.clip) {
			return ReleaseAnswer::Held;
		}
	}
	// Focus (heldPart_, lockedPart_, thinDrivePart_) is part indices, not pointers: nothing to let go of.
	return ReleaseAnswer::Released;
}

ModelStackWithAutoParam* PerformView::leadParam(int32_t part, SoundControl control, char* memory) {
	const auto* genre = performAdapter.steering().rosterGenre();
	if (!performAdapter.active() || genre == nullptr) {
		return nullptr;
	}
	return soundControls.leadParam(performAdapter.output().roster, *genre, part, control, memory);
}

void PerformView::readControls(int32_t part, ControlLevel levels[deluge::generative::kNumSoundControls]) {
	for (int32_t c = 0; c < deluge::generative::kNumSoundControls; c++) {
		char memory[MODEL_STACK_MAX_SIZE];
		ModelStackWithAutoParam* stack = leadParam(part, static_cast<SoundControl>(c), memory);
		levels[c] = ControlLevel{};
		if (stack != nullptr) {
			// The live value is the param now: home plus every layer (spec §13.5).
			levels[c].has = true;
			levels[c].home = static_cast<int8_t>(knobPos(*stack, soundControls.homeOf(*stack)));
			levels[c].live = static_cast<int8_t>(liveKnobPos(*stack));
		}
	}
}

ActionResult PerformView::clipPadAction(int32_t x, int32_t y, bool on) {
	using namespace deluge::generative::perform;
	const int32_t row = rowFromY(y);
	const int32_t part = shownPart(); // with none, the page does nothing
	if (!on || part < 0) {
		return ActionResult::DEALT_WITH;
	}
	if (row == kTakesRow) {
		// Tapping a lit take queues it for the next phrase line; the one queued, or the one playing, cancels it.
		if (takeTappable(performAdapter.steering().takeRow(part), x)) {
			if (!refusedWithPopup(performAdapter.steering().pressTake(static_cast<uint8_t>(part), x))) {
				TEST_EVENT("gen_press take part=%ld take=%ld", static_cast<long>(part), static_cast<long>(x));
			}
		}
		return ActionResult::DEALT_WITH;
	}
	if (row == kActionsRow) {
		if (x == kFreezePad) {
			freezePress(part);
		}
		else if (x == kNewSoundPad) {
			soundPress(part, Buttons::isShiftButtonPressed());
		}
		else if (x == kSoundEditorPad) {
			return openSoundEditorRootMenu(part); // the shown part's root menu (spec §13.7)
		}
		return ActionResult::DEALT_WITH;
	}
	char memory[MODEL_STACK_MAX_SIZE];
	ModelStackWithAutoParam* stack = leadParam(part, faderControl(row), memory);
	if (stack == nullptr) {
		return ActionResult::DEALT_WITH;
	}
	// A tap sets home (spec §13.5): the param plays it plus the control's offset now, and the edit is logged.
	setLeadParam(*stack, faderKnobPos(x), true);
	return ActionResult::DEALT_WITH;
}

namespace {
/// A sound press's popup, if refused, and its test event (`kind` is the event's word).
void showSoundPress(int32_t part, const char* kind, bool previous, SoundSwaps::Press press) {
	if (press == SoundSwaps::Press::NoPrevious) {
		display->displayPopup(deluge::l10n::get(deluge::l10n::String::STRING_FOR_NO_PREVIOUS_SOUND));
	}
	refusedWithPopup(SoundSwaps::outcome(press));
	TEST_EVENT("gen_press %s part=%ld previous=%d result=%d working=%d", kind, static_cast<long>(part),
	           previous ? 1 : 0, static_cast<int>(press), workingAnimationShown() ? 1 : 0);
}
} // namespace

void PerformView::soundPress(int32_t part, bool previous) {
	// The stock working animation while it loads, so a refused card action makes sense (spec §12.10).
	showSoundPress(part, "sound", previous, pressShowingWork([&] { return soundSwaps.press(part, previous); }));
	refreshClipPage();
}

void PerformView::openPresetBrowser(int32_t part) {
	if (currentUIMode != UI_MODE_NONE) {
		return;
	}
	if (soundSwaps.pending(part)) {
		soundPress(part, false); // SYNTH again cancels the part's sound (spec §10.7)
		return;
	}
	const SoundSwaps::Press press = soundSwaps.canStart(part);
	if (press != SoundSwaps::Press::Preloading) {
		showSoundPress(part, "browser", false, press); // refused as a re-roll would be (spec §12.9, §12.10)
		return;
	}
	// A held row lets go once the browser has opened (a failed open leaves Perform on top). A held End pad needs
	// nothing: the browser is the current UI from the start of its opened(), which reads the card, so a release
	// delivered meanwhile isn't a tap (endPadActs()); it still lets go of the hold.
	if (performPresetBrowser.open(part)) {
		letGoOfHeldPart();
	}
}

void PerformView::chooseSound(int32_t part, const SoundChoice& choice) {
	showSoundPress(part, "chosen", false, pressShowingWork([&] { return soundSwaps.choose(part, choice); }));
	refreshClipPage();
}

void PerformView::freezePress(int32_t part) {
	refusedWithPopup(performAdapter.steering().pressFreeze(static_cast<uint8_t>(part)));
	TEST_EVENT("gen_press freeze part=%ld", static_cast<long>(part));
}

void PerformView::showKeyAndSeed() {
	// The key and scale playing, and the seed code (spec §6.5).
	using namespace deluge::generative::perform;
	const auto& recipe = currentRecipe.recipe;
	char seed[kSeedCodeChars + 1];
	seedCode(currentRecipe.seed, seed);
	char text[40];
	snprintf(text, sizeof(text), "%s %s %s", keyName(recipe.keyRoot), scaleName(recipe.scale), seed);
	display->displayPopup(text);
	TEST_EVENT("gen_seed_popup %s", text);
}

int32_t PerformView::shownPart() const {
	return lastFocusedPart_ >= 0 && lastFocusedPart_ < performAdapter.steering().rosterParts() ? lastFocusedPart_ : -1;
}

bool PerformView::partFrozen(int32_t part) {
	const Generator* piece = performAdapter.playingGenerator();
	return piece != nullptr && part >= 0 && part < performAdapter.steering().rosterParts() && piece->partFrozen(part);
}

ActionResult PerformView::verticalEncoderAction(int32_t offset, bool /*inCardRoutine*/) {
	// Y turn: the focused part's take back or forward, from the one queued or else the one playing (spec §13.5).
	// SHIFT + Y turn: its octave shift down or up instead.
	const int32_t part = focusedPart();
	if (part < 0 || offset == 0) {
		return ActionResult::DEALT_WITH;
	}
	if (Buttons::isShiftButtonPressed()) {
		octaveTurn(part, offset);
		return ActionResult::DEALT_WITH;
	}
	PressOutcome outcome = PressOutcome::NothingToDo;
	const int32_t take = performAdapter.steering().stepTake(static_cast<uint8_t>(part),
	                                                        performAdapter.steering().takeRow(part), offset, &outcome);
	refusedWithPopup(outcome);
	if (take >= 0) { // none that way, or refused: -1
		TEST_EVENT("gen_press take part=%ld take=%ld y=%ld", static_cast<long>(part), static_cast<long>(take),
		           static_cast<long>(offset));
		refreshClipPage();
	}
	return ActionResult::DEALT_WITH;
}

void PerformView::octaveTurn(int32_t part, int32_t offset) {
	// Drums and FX have no Register: nothing to do, and no popup (spec §13.5).
	if (octaveEdgePopup(part, offset)) {
		return;
	}
	PressOutcome outcome = PressOutcome::NothingToDo;
	const int32_t shift = performAdapter.steering().stepOctave(static_cast<uint8_t>(part), offset, &outcome);
	if (refusedWithPopup(outcome) || shift == deluge::generative::kNoOctaveWaiting) {
		return;
	}
	const deluge::generative::Genre* genre = performAdapter.steering().rosterGenre();
	char text[deluge::generative::perform::kQueueLineChars + 1];
	deluge::generative::perform::queueLabel(
	    {deluge::generative::CommandKind::SetOctave, static_cast<uint8_t>(part), shift}, *genre, text);
	display->displayPopup(text);
	TEST_EVENT("gen_press octave part=%ld shift=%ld y=%ld", static_cast<long>(part), static_cast<long>(shift),
	           static_cast<long>(offset));
	TEST_EVENT("gen_octave_popup %s", text);
}

bool PerformView::octaveEdgePopup(int32_t part, int32_t direction) {
	if (!performAdapter.steering().octaveAtEdge(part, direction)) {
		return false;
	}
	// The shift as it goes on is the lowest or highest the part takes: refused, and the popup says so (spec §13.5).
	const int32_t shift = performAdapter.steering().octaveAhead(part);
	char text[deluge::generative::perform::kQueueLineChars + 16];
	deluge::generative::perform::queueLabel(
	    {deluge::generative::CommandKind::SetOctave, static_cast<uint8_t>(part), shift},
	    *performAdapter.steering().rosterGenre(), {text, deluge::generative::perform::kQueueLineChars + 1});
	const l10n::String edge =
	    direction < 0 ? l10n::String::STRING_FOR_OCTAVE_LOWEST : l10n::String::STRING_FOR_OCTAVE_HIGHEST;
	strncat(text, " ", sizeof(text) - strlen(text) - 1);
	strncat(text, l10n::get(edge), sizeof(text) - strlen(text) - 1);
	display->displayPopup(text);
	TEST_EVENT("gen_octave_popup %s", text);
	return true;
}

void PerformView::refreshFaders() {
	fadersAt_ = AudioEngine::audioSampleTimer;
#if !ENABLE_SYSEX_TEST
	if (page_ != Page::Clip) {
		return;
	}
#endif
	// Test builds read the faders on every page, for the probe scripts.
	readControls(lastFocusedPart_, faderLevels_);
	refreshClipPage(true);
}

namespace {
/// A CLIP page cell's colour now on a part of `colour`, its role's: blinking included.
RGB clipColour(const deluge::generative::perform::ClipCell& cell, RGB colour, bool blinkOff) {
	using Look = deluge::generative::perform::ClipCell::Look;
	switch (cell.look) {
	// Row 1, the takes: bright playing, dim earlier, grey the next new take, the queued one blinking.
	case Look::TakePlaying:
		return colour;
	case Look::TakeEarlier:
		return colour.dim(3);
	case Look::TakeNew:
		return RGB::monochrome(40);
	case Look::TakeQueued:
		return blinkOff ? deluge::gui::colours::black : colour;
	// Row 2: FREEZE (bright while frozen), NEW SOUND (blinking while it preloads or waits for its line), SOUND
	// EDITOR.
	case Look::Freeze:
		return cell.lit ? kFrozenColour : kFrozenColour.dim(3);
	case Look::NewSound:
		return kNewSoundColour;
	case Look::SoundLoading:
	case Look::SoundWaiting:
		return blinkOff ? deluge::gui::colours::black : kNewSoundColour;
	case Look::SoundEditor:
		return kSoundEditorColour;
	// Rows 3–8, the faders.
	case Look::FaderLive:
		return RGB(255, 255, 255);
	case Look::Fader:
		return cell.lit ? colour.dim(1) : colour.dim(4);
	case Look::Off:
		break;
	}
	return deluge::gui::colours::black;
}
} // namespace

void PerformView::refreshClipPage([[maybe_unused]] bool reportFaders) {
	using namespace deluge::generative::perform;
	static_assert(ClipPage::kRows == kDisplayHeight && ClipPage::kColumns == kDisplayWidth);
#if !ENABLE_SYSEX_TEST
	// As in refreshFaders(), and needed here too: refresh() and the take and sound actions call this directly.
	if (page_ != Page::Clip) {
		return;
	}
#endif
	// Test builds compose the page on every page, to describe it for the probe scripts.
	ClipPageInput in;
	in.part = lastFocusedPart_;
	in.parts = performAdapter.steering().rosterParts();
	in.takes = performAdapter.steering().takeRow(in.part);
	in.frozen = partFrozen(in.part);
	// soundSwaps keeps a sound per roster part, so it is asked only about one.
	const bool inRoster = in.part >= 0 && in.part < in.parts;
	in.sound = !inRoster                        ? SoundSwap::None
	           : soundSwaps.preloading(in.part) ? SoundSwap::Loading
	           : soundSwaps.waiting(in.part)    ? SoundSwap::Waiting
	                                            : SoundSwap::None;
	std::copy_n(faderLevels_, deluge::generative::kNumSoundControls, in.levels); // as refreshFaders() last read them
	ClipPage composed;
	composeClipPage(in, composed);
#if ENABLE_SYSEX_TEST
	{
		char text[kClipTextChars + 1];
		static_assert(sizeof(text) == sizeof(shownFaders_) && sizeof(text) == sizeof(shownClip_));
		if (reportFaders) {
			describeFaders(composed, text);
			if (strcmp(text, shownFaders_) != 0) {
				strcpy(shownFaders_, text);
				TEST_EVENT("gen_faders %s", text);
			}
		}
		describeClip(composed, text);
		if (strcmp(text, shownClip_) != 0) {
			strcpy(shownClip_, text);
			TEST_EVENT("gen_clip %s", text);
		}
	}
	if (page_ != Page::Clip) {
		return;
	}
#endif
	RGB pads[kDisplayHeight][kDisplayWidth] = {};
	if (composed.shown) {
		const RGB colour =
		    kRoleColours[static_cast<int32_t>(performAdapter.steering().rosterGenre()->parts[in.part].role)];
		const bool blinkOff = (AudioEngine::audioSampleTimer / kBlinkSamples) % 2 != 0;
		for (int32_t y = 0; y < kDisplayHeight; y++) {
			for (int32_t x = 0; x < kDisplayWidth; x++) {
				pads[y][x] = clipColour(composed.cell[rowFromY(y)][x], colour, blinkOff);
			}
		}
	}
	showMainPads(pads);
}
