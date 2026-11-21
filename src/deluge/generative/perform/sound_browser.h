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

// Perform's sound browsers (spec §13.3, §13.5): the preset browser on a synth part and the lane browser on a Drums or
// FX lane. Where each opens, and whether it previews the file under the cursor (BROWSER PREVIEW).

#pragma once

#include "generative/core/palette.h"
#include "generative/core/recipe.h"
#include <cstdint>

namespace deluge::generative::perform {

/// The Perform BROWSER PREVIEW setting (spec §13.3): whether the browsers play the file under the cursor. Kept until
/// power-off.
enum class BrowserPreview : uint8_t { Off, WhenStopped, Always };
constexpr BrowserPreview kDefaultBrowserPreview = BrowserPreview::WhenStopped;

/// Whether a browser previews under `setting`, with the transport `playing`.
[[nodiscard]] constexpr bool previews(BrowserPreview setting, bool playing) {
	return setting == BrowserPreview::Always || (setting == BrowserPreview::WhenStopped && !playing);
}

/// What a browser lists: card presets, or audio files.
enum class BrowseKind : uint8_t { Preset, Sample };

/// Where a browser opens (spec §13.5): in the pick's folder on the pick, when the pick is of the kind it lists, else
/// in `fallback`, which it also goes to when the pick's folder is gone.
struct BrowserStart {
	SoundPath folder;
	/// The file selected in `folder`: a pointer into the pick, or nullptr.
	const char* name = nullptr;
	/// SYNTHS for a part's preset, the lane's first preset's folder (else SYNTHS) for an FX lane's, and the lane's
	/// first palette folder for its sample.
	SoundPath fallback;
};

/// Where a browser of `kind` for `pick` opens: a synth part's preset browser (`lane` None) or `lane`'s browser,
/// with `palette` the Current Recipe's. `pick` must outlive the result.
[[nodiscard]] BrowserStart browserStart(const Palette& palette, Lane lane, const SoundPath& pick, BrowseKind kind);

} // namespace deluge::generative::perform
