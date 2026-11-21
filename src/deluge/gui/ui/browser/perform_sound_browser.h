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

// Perform's sound browser (spec §13.5, §10.7): SYNTH on a focused synth part opens it on the part's presets, and a
// lane of the lane list (KIT on a Drums or FX part) on the lane's samples, or on presets for an FX lane that takes one.
// It is the stock browser's file list, folder navigation and QWERTY search over every matching file on the card, but
// scrolling never loads into the part: it plays on with its sound. BROWSER PREVIEW (spec §13.3) says whether a sample
// under the cursor previews, at the lane's volume, and whether a preset does (stopped only), on an Output built beside
// the part's (firmware::presetPreview). SELECT on a file hands it to Perform, which preloads it as a
// re-roll and swaps it in at the next phrase line; BACK at the top folder closes it with nothing changed, back to the
// lane list if it opened from one. Playback, the gold knobs and the sidebar stay Perform's meanwhile (End aside, as
// under every overlay), and Perform's frame runs underneath, so swaps, transitions and retirements are serviced.

#pragma once

#include "generative/core/types.h"
#include "generative/perform/sound_browser.h"
#include "gui/ui/browser/slot_browser.h"

class FileItem;

class PerformSoundBrowser final : public SlotBrowser {
public:
	PerformSoundBrowser() = default;

	/// Opens over the current UI on `part`'s pick of `kind`: its preset (`lane` None, a synth part), or `lane`'s file.
	/// It opens where perform::browserStart() says. False if the card or the folder can't be read: the stock error
	/// shows, and the UI underneath stays on top.
	bool open(int32_t part, deluge::generative::Lane lane, deluge::generative::perform::BrowseKind kind);

	/// The BROWSER PREVIEW setting (spec §13.3), kept until power-off.
	[[nodiscard]] static deluge::generative::perform::BrowserPreview previewSetting() { return preview_; }
	static void setPreviewSetting(deluge::generative::perform::BrowserPreview setting) { preview_ = setting; }

	bool opened() override;
	void focusRegained() override;
	ActionResult buttonAction(deluge::hid::Button b, bool on, bool inCardRoutine) override;
	ActionResult padAction(int32_t x, int32_t y, int32_t velocity) override;
	ActionResult timerCallback() override;
	void graphicsRoutine() override;
	UIType getUIType() override { return UIType::GENERATIVE_SOUND_BROWSER; }
	/// A sample's path is its folder and file name as the card has it; a preset's is the slot browser's.
	Error getCurrentFilePath(String* path) override;

protected:
	void enterKeyPress() override;
	void currentFileChanged(int32_t movementDirection) override;
	void folderContentsReady(int32_t entryDirection) override;
	void exitAction() override;

private:
	/// Closes the browser, its LED back off and any preview silenced. A choice also closes the lane list under it.
	void closeBrowser(bool chosen);
	/// Previews the file under the cursor, if BROWSER PREVIEW lets it now, else silences the last one: a sample through
	/// stock's preview voice, a preset loaded beside the part with one note (spec §13.3).
	void preview();
	void previewPreset(FileItem& item, bool playing);
	[[nodiscard]] bool forLane() const { return lane_ != deluge::generative::Lane::None; }
	[[nodiscard]] bool samples() const { return kind_ == deluge::generative::perform::BrowseKind::Sample; }

	int32_t part_ = -1;
	deluge::generative::Lane lane_ = deluge::generative::Lane::None;
	deluge::generative::perform::BrowseKind kind_ = deluge::generative::perform::BrowseKind::Preset;
	/// Opened over the lane list: a choice closes it too.
	bool overList_ = false;
	/// SELECT on a file came while a preset preview still built: the choice is made once it has unwound.
	bool chooseWhenFree_ = false;
	/// The transport played at the last frame: a stop then previews the preset under the cursor.
	bool wasPlaying_ = false;
	static inline deluge::generative::perform::BrowserPreview preview_ =
	    deluge::generative::perform::kDefaultBrowserPreview;
};

extern PerformSoundBrowser performSoundBrowser;
