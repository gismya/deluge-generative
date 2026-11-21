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

#include "gui/ui/browser/perform_sound_browser.h"
#include "definitions_cxx.hpp"
#include "generative/core/lane_names.h"
#include "generative/core/palette_file.h"
#include "generative/core/recipe.h"
#include "generative/firmware/current_recipe.h"
#include "generative/firmware/perform_adapter.h"
#include "generative/firmware/sound_swaps.h"
#include "gui/l10n/l10n.h"
#include "gui/ui/browser/sample_browser.h"
#include "gui/ui/generative_popups.h"
#include "gui/ui/sound_editor.h"
#include "gui/views/perform_view.h"
#include "hid/button.h"
#include "hid/display/display.h"
#include "hid/display/oled.h"
#include "hid/led/indicator_leds.h"
#include "io/midi/test_port.h"
#include "model/clip/instrument_clip.h"
#include "model/note/note_row.h"
#include "playback/playback_handler.h"
#include "processing/engines/audio_engine.h"
#include "processing/sound/sound_drum.h"
#include "storage/file_item.h"

PerformSoundBrowser performSoundBrowser{};

namespace gen = deluge::generative;
namespace perform = deluge::generative::perform;
using gen::firmware::currentRecipe;
using gen::firmware::performAdapter;
using gen::firmware::SoundSwaps;
using gen::firmware::soundSwaps;

namespace {

/// The audio files a lane's sample browser lists, as stock's sample browser does.
char const* audioExtensions[] = {"WAV", "AIFF", "AIF", nullptr};

/// The pick a browser for `part` (and `lane`, None for a synth part's preset) opens on.
const gen::SoundPath& pickOf(int32_t part, gen::Lane lane) {
	return lane == gen::Lane::None ? currentRecipe.sounds.part[part]
	                               : currentRecipe.sounds.lane[static_cast<int32_t>(lane)];
}

/// The param managers of the Sound `lane`'s row plays in `part`'s Kit, and of the Kit's clip, for the preview's
/// volume. Nulls when the roster has none.
struct LaneParams {
	ParamManagerForTimeline* row = nullptr;
	ParamManagerForTimeline* kit = nullptr;
};
LaneParams laneParams(int32_t part, gen::Lane lane) {
	const gen::firmware::PartOutput& output = performAdapter.output().roster.parts[part];
	Drum* drum = output.drums[gen::laneRow(lane)];
	if (output.clip == nullptr || drum == nullptr) {
		return {};
	}
	NoteRow* row = output.clip->getNoteRowForDrum(drum);
	if (row == nullptr) {
		return {};
	}
	return {.row = &row->paramManager, .kit = &output.clip->paramManager};
}

} // namespace

bool PerformSoundBrowser::open(int32_t part, gen::Lane lane, perform::BrowseKind kind) {
	part_ = part;
	lane_ = lane;
	kind_ = kind;
	overList_ = getCurrentUI() == &soundEditor;
	display->setNextTransitionDirection(1);
	return openUI(this);
}

bool PerformSoundBrowser::opened() {
	Error error = beginSlotSession();
	if (error == Error::NONE) {
		title = forLane() ? gen::laneName(lane_) : deluge::l10n::get(deluge::l10n::String::STRING_FOR_CHOOSE_SOUND);
		fileIcon = samples() ? deluge::hid::display::OLED::waveIcon : deluge::hid::display::OLED::synthIcon;
		fileIconPt2 = nullptr;
		fileIconPt2Width = 0;
		filePrefix = samples() ? nullptr : "SYNT";
		if (samples()) {
			allowedFileExtensions = audioExtensions;
			allowFoldersSharingNameWithFile = true;
		}
		// A card listing only: the stock browser also lists the song's Instruments, and Perform's are its parts.
		outputTypeToLoad = OutputType::NONE;
		qwertyVisible = true;
		enteredText.clear();

		// Start in the pick's folder, on the pick (spec §13.5).
		const perform::BrowserStart start =
		    perform::browserStart(currentRecipe.palette(), lane_, pickOf(part_, lane_), kind_);
		// The pick's folder, the palette's while that's gone, and the top folder while that's gone too: the stock
		// browser makes a missing default folder, so only the top one is passed as that.
		error = currentDir.set(start.folder.c_str());
		if (error == Error::NONE) {
			error = arrivedInNewFolder(0, start.name, nullptr);
		}
		if (error == Error::FOLDER_DOESNT_EXIST) {
			error = currentDir.set(start.paletteFolder.c_str());
			if (error == Error::NONE) {
				error = arrivedInNewFolder(0, nullptr, start.topFolder);
			}
		}
	}
	if (error != Error::NONE) {
		display->displayError(error);
		TEST_EVENT("gen_browser closed error=%d", static_cast<int>(error));
		return false;
	}
	focusRegained();
#if ENABLE_SYSEX_TEST
	FileItem* item = getCurrentFileItem();
	if (forLane()) {
		// No pick= (an event holds 120 characters): at= is the pick when the browser found it.
		TEST_EVENT("gen_browser open part=%ld lane=%s kind=%s at=%s/%s", static_cast<long>(part_),
		           gen::lanePaletteName(lane_), samples() ? "sample" : "preset", currentDir.get(),
		           item != nullptr ? item->filename.get() : "");
	}
	else {
		TEST_EVENT("gen_browser open part=%ld pick=%s at=%s/%s", static_cast<long>(part_), pickOf(part_, lane_).c_str(),
		           currentDir.get(), item != nullptr ? item->filename.get() : "");
	}
#endif
	return true; // arrivedInNewFolder() has previewed the file it selected (folderContentsReady())
}

void PerformSoundBrowser::focusRegained() {
	indicator_leds::blinkLed(forLane() ? IndicatorLED::KIT : IndicatorLED::SYNTH);
	SlotBrowser::focusRegained();
}

ActionResult PerformSoundBrowser::buttonAction(deluge::hid::Button b, bool on, bool inCardRoutine) {
	using namespace deluge::hid::button;
	if (b == SYNC_SCALING) {
		return performView.buttonAction(b, on, inCardRoutine); // FILL while held, as under Perform's menus (spec §8.7)
	}
	if (b == SYNTH || b == KIT || b == SAVE || b == LOAD) {
		// Not the stock browser's type switch, file delete or load: Perform's own meanings wait for it to close.
		return ActionResult::DEALT_WITH;
	}
	return SlotBrowser::buttonAction(b, on, inCardRoutine);
}

ActionResult PerformSoundBrowser::padAction(int32_t x, int32_t y, int32_t velocity) {
	if (x >= kDisplayWidth) {
		// The sidebar stays Perform's: mutes, solos, forces and the hold. End can't arm under it, as under every
		// overlay (spec §13.5).
		return performView.padAction(x, y, velocity);
	}
	return SlotBrowser::padAction(x, y, velocity);
}

ActionResult PerformSoundBrowser::timerCallback() {
	if (currentUIMode == UI_MODE_HOLDING_BUTTON_POTENTIAL_LONG_PRESS) {
		return ActionResult::DEALT_WITH; // no stock context menu: SELECT's release chooses, held or not
	}
	return SlotBrowser::timerCallback();
}

void PerformSoundBrowser::graphicsRoutine() {
	// Perform's frame: swaps, transitions and retirements are serviced while browsing, and the sidebar is live.
	performView.graphicsRoutine();
}

void PerformSoundBrowser::currentFileChanged(int32_t movementDirection) {
	// Nothing loads into the part: it plays on with its sound until a file is chosen (spec §13.5).
#if ENABLE_SYSEX_TEST
	FileItem* item = getCurrentFileItem();
	TEST_EVENT("gen_browser at=%s/%s", currentDir.get(), item != nullptr ? item->filename.get() : "");
#endif
	preview();
}

void PerformSoundBrowser::folderContentsReady(int32_t entryDirection) {
	currentFileChanged(0);
}

Error PerformSoundBrowser::getCurrentFilePath(String* path) {
	FileItem* item = getCurrentFileItem();
	if (!samples() || item == nullptr) {
		return SlotBrowser::getCurrentFilePath(path); // the entered name with .XML
	}
	path->set(&currentDir);
	Error error = path->concatenate("/");
	if (error == Error::NONE) {
		error = path->concatenate(&item->filename);
	}
	return error;
}

void PerformSoundBrowser::preview() {
	AudioEngine::stopAnyPreviewing();
	FileItem* item = getCurrentFileItem();
	if (!samples() || item == nullptr || item->isFolder) {
		return;
	}
	const bool playing = playbackHandler.isEitherClockActive();
	if (!perform::previews(preview_, playing)) {
		TEST_EVENT("gen_browser_preview off playing=%d", playing ? 1 : 0);
		return;
	}
	String path;
	if (getCurrentFilePath(&path) != Error::NONE) {
		return;
	}
	// At the lane's volume, as stock previews at its kit row's (spec §13.3).
	const LaneParams params = laneParams(part_, lane_);
	AudioEngine::previewSample(&path, &item->filePointer, true, previewVolumeFor(params.row, params.kit));
	TEST_EVENT("gen_browser_preview sounding=%d playing=%d path=%s",
	           AudioEngine::sampleForPreview->hasActiveVoices() ? 1 : 0, playing ? 1 : 0, path.get());
}

void PerformSoundBrowser::enterKeyPress() {
	FileItem* item = getCurrentFileItem();
	if (item == nullptr) {
		return;
	}
	if (item->isFolder) {
		AudioEngine::stopAnyPreviewing();
		Error error = goIntoFolder(item->filename.get());
		if (error != Error::NONE) {
			display->displayError(error);
			TEST_EVENT("gen_browser closed error=%d", static_cast<int>(error));
			closeBrowser(false);
		}
		return;
	}
	gen::SoundChoice choice{.lane = lane_};
	if (!choice.path.set(currentDir.get(), item->filename.get())) {
		display->displayPopup(deluge::l10n::get(deluge::l10n::String::STRING_FOR_PATH_TOO_LONG));
		return; // a pick holds no longer path (spec §10.1): choose another
	}
	// Refused as choose() would refuse it (the runner busy freeing a swapped-out sound, a transition pending): the
	// browser stays open on the file, to choose again (spec §13.5).
	const SoundSwaps::Press press = soundSwaps.canChoose(part_, choice);
	if (press != SoundSwaps::Press::Preloading) {
		deluge::gui::refusedWithPopup(SoundSwaps::outcome(press));
		TEST_EVENT("gen_browser refused part=%ld result=%d", static_cast<long>(part_), static_cast<int>(press));
		return;
	}
	const int32_t part = part_;
	if (forLane()) {
		TEST_EVENT("gen_browser chose part=%ld path=%s lane=%s", static_cast<long>(part), choice.path.c_str(),
		           gen::lanePaletteName(lane_));
	}
	else {
		TEST_EVENT("gen_browser chose part=%ld path=%s", static_cast<long>(part), choice.path.c_str());
	}
	closeBrowser(true);
	performView.chooseSound(part, choice);
}

void PerformSoundBrowser::exitAction() {
	TEST_EVENT("gen_browser closed");
	closeBrowser(false);
}

void PerformSoundBrowser::closeBrowser(bool chosen) {
	AudioEngine::stopAnyPreviewing();
	indicator_leds::setLedState(forLane() ? IndicatorLED::KIT : IndicatorLED::SYNTH, false);
	const bool overList = overList_;
	part_ = -1;
	lane_ = gen::Lane::None;
	overList_ = false;
	close();
	if (chosen && overList && getCurrentUI() == &soundEditor) {
		soundEditor.exitCompletely(); // the lane list goes too: Perform is on top for the press's popup
	}
}
