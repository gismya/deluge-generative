// Perform's sound browsers (spec §13.3, §13.5): where the preset browser and the lane browser open, and when they
// preview the file under the cursor (BROWSER PREVIEW).

#include "generative/core/genre.h"
#include "generative/core/palette.h"
#include "generative/perform/sound_browser.h"

#include "cppspec.hpp"

#include <string>

using namespace deluge::generative;
using namespace deluge::generative::perform;

namespace {

constexpr const char* kFallFolders[] = {"SAMPLES/MY FX/Falls", "SAMPLES/FX"};
constexpr PaletteEntry kFallSynths[] = {{"SYNTHS/GENERATIVE/FX/GEN Fall.XML"}};
constexpr PaletteEntry kLooseSynths[] = {{"GEN Siren.XML"}}; // a preset at the card's root
constexpr LaneSounds kLanes[] = {
    {.lane = Lane::Fall, .folders = kFallFolders, .synths = kFallSynths},
    {.lane = Lane::Riser},
    {.lane = Lane::Siren, .synths = kLooseSynths},
};
constexpr Palette kPalette{.lanes = kLanes};

SoundPath pathOf(const char* text) {
	SoundPath path;
	path.set(text);
	return path;
}

std::string text(const SoundPath& path) {
	return path.c_str();
}

std::string text(const char* name) {
	return name != nullptr ? name : "(none)";
}

} // namespace

// clang-format off
describe sound_browser("Perform's sound browsers", $ {
	context("BROWSER PREVIEW", _ {
		it("defaults to WHEN STOPPED", _ {
			expect(kDefaultBrowserPreview == BrowserPreview::WhenStopped).to_be_true();
		});

		it("previews never when OFF, only stopped when WHEN STOPPED, and always when ALWAYS", _ {
			expect(previews(BrowserPreview::Off, false)).to_be_false();
			expect(previews(BrowserPreview::Off, true)).to_be_false();
			expect(previews(BrowserPreview::WhenStopped, false)).to_be_true();
			expect(previews(BrowserPreview::WhenStopped, true)).to_be_false();
			expect(previews(BrowserPreview::Always, false)).to_be_true();
			expect(previews(BrowserPreview::Always, true)).to_be_true();
		});
	});

	context("where it opens", _ {
		it("opens a synth part's preset browser on its pick, or in SYNTHS", _ {
			const SoundPath pick = pathOf("SYNTHS/GENERATIVE/GEN Acid.XML");
			BrowserStart start = browserStart(kPalette, Lane::None, pick, BrowseKind::Preset);
			expect(text(start.folder)).to_equal(std::string("SYNTHS/GENERATIVE"));
			expect(text(start.name)).to_equal(std::string("GEN Acid.XML"));
			expect(text(start.paletteFolder)).to_equal(std::string("SYNTHS"));
			expect(text(start.topFolder)).to_equal(std::string("SYNTHS"));

			start = browserStart(kPalette, Lane::None, SoundPath{}, BrowseKind::Preset);
			expect(text(start.folder)).to_equal(std::string("SYNTHS"));
			expect(text(start.name)).to_equal(std::string("(none)"));
		});

		it("opens a lane's sample browser on its sample, then the lane's first palette folder, then SAMPLES", _ {
			const SoundPath pick = pathOf("MY SAMPLES/Hits/Boom.wav");
			BrowserStart start = browserStart(kPalette, Lane::Fall, pick, BrowseKind::Sample);
			expect(text(start.folder)).to_equal(std::string("MY SAMPLES/Hits"));
			expect(text(start.name)).to_equal(std::string("Boom.wav"));
			expect(text(start.paletteFolder)).to_equal(std::string("SAMPLES/MY FX/Falls"));
			expect(text(start.topFolder)).to_equal(std::string("SAMPLES"));
		});

		it("falls back to the lane's default folder when its palette entry lists none, or it has no entry", _ {
			BrowserStart riser = browserStart(kPalette, Lane::Riser, SoundPath{}, BrowseKind::Sample);
			expect(text(riser.folder)).to_equal(std::string(defaultLaneFolders(Lane::Riser)[0]));
			expect(text(riser.name)).to_equal(std::string("(none)"));
			BrowserStart kick = browserStart(kPalette, Lane::Kick, SoundPath{}, BrowseKind::Sample);
			expect(text(kick.folder)).to_equal(std::string(defaultLaneFolders(Lane::Kick)[0]));
		});

		it("opens on an FX lane's pick only when it is of the kind listed: a preset pick isn't a sample", _ {
			const SoundPath preset = pathOf("SYNTHS/MINE/Boom.XML");
			BrowserStart samples = browserStart(kPalette, Lane::Fall, preset, BrowseKind::Sample);
			expect(text(samples.folder)).to_equal(std::string("SAMPLES/MY FX/Falls"));
			expect(text(samples.name)).to_equal(std::string("(none)"));
			BrowserStart presets = browserStart(kPalette, Lane::Fall, preset, BrowseKind::Preset);
			expect(text(presets.folder)).to_equal(std::string("SYNTHS/MINE"));
			expect(text(presets.name)).to_equal(std::string("Boom.XML"));

			const SoundPath sample = pathOf("SAMPLES/FX/Fall.wav");
			BrowserStart fromSample = browserStart(kPalette, Lane::Fall, sample, BrowseKind::Preset);
			expect(text(fromSample.folder)).to_equal(std::string("SYNTHS/GENERATIVE/FX"));
			expect(text(fromSample.name)).to_equal(std::string("(none)"));
		});

		it("falls back to SYNTHS for an FX lane's presets when its first preset sits at the card's root", _ {
			BrowserStart siren = browserStart(kPalette, Lane::Siren, SoundPath{}, BrowseKind::Preset);
			expect(text(siren.folder)).to_equal(std::string("SYNTHS"));
			expect(text(siren.paletteFolder)).to_equal(std::string("SYNTHS"));
		});

		it("opens in each genre's palette folder for every lane it plays, never an empty one", _ {
			for (const Genre* genre : {&genres::technoAcid, &genres::dub, &genres::house, &genres::ambient,
			                           &genres::psytranceClassic}) {
				for (const PartSpec& part : genre->parts) {
					for (const LaneSpec& lane : part.lanes) {
						for (BrowseKind kind : {BrowseKind::Sample, BrowseKind::Preset}) {
							BrowserStart start = browserStart(*genre->palette, lane.lane, SoundPath{}, kind);
							expect(start.folder.empty()).to_be_false();
							expect(text(start.folder)).to_equal(text(start.paletteFolder));
							expect(text(start.topFolder))
							    .to_equal(std::string(kind == BrowseKind::Sample ? "SAMPLES" : "SYNTHS"));
						}
					}
				}
			}
		});
	});
});

CPPSPEC_SPEC(sound_browser)
