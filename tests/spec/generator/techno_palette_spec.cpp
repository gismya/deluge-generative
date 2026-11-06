// Techno's built-in palette against the factory card plus the Generative pack (spec §10.1–§10.5, §18.3).

#include "fake_card.h"
#include "generative/core/genre.h"
#include "generative/core/palette.h"
#include "generative/core/recipe.h"

#include "cppspec.hpp"

#include <map>
#include <set>
#include <string>

using namespace deluge::generative;
using harness::FakeCard;

namespace {

const Genre& techno = genres::techno;

/// Every distinct pick per lane over many fresh rolls.
std::map<Lane, std::set<std::string>> lanePicks(FakeCard& card) {
	std::map<Lane, std::set<std::string>> picks;
	for (uint32_t entropy = 0; entropy < 800; entropy++) {
		Recipe recipe = newRecipe(techno, entropy);
		rollSounds(recipe, entropy, card);
		for (int32_t l = 0; l < kNumLanes; l++) {
			if (!recipe.sounds.lane[l].empty()) {
				picks[static_cast<Lane>(l)].insert(recipe.sounds.lane[l].c_str());
			}
		}
	}
	return picks;
}

} // namespace

// clang-format off
describe techno_palette("Techno's palette", $ {
	it("gives every synth part at least 3 presets, each on the factory card or in the pack", _ {
		FakeCard card = harness::factoryCardWithPack();
		expect(techno.palette != nullptr).to_be_true();
		for (const PartSpec& part : techno.parts) {
			if (!part.lanes.empty()) {
				continue;
			}
			auto entries = techno.palette->partEntries(part);
			expect(entries.size()).to_be_greater_than(size_t{2});
			for (const PaletteEntry& entry : entries) {
				expect(card.has(entry.path)).to_be_true();
			}
		}
	});

	it("gives every lane Techno plays at least 3 files after tag filtering", _ {
		FakeCard card = harness::factoryCardWithPack();
		auto picks = lanePicks(card);
		for (const PartSpec& part : techno.parts) {
			for (const LaneSpec& lane : part.lanes) {
				expect(picks[lane.lane].size()).to_be_greater_than(size_t{2});
			}
		}
	});

	it("plays an FX part whose Riser picks only pack risers", _ {
		bool hasFx = false;
		for (const PartSpec& part : techno.parts) {
			hasFx = hasFx || part.role == Role::FX;
		}
		expect(hasFx).to_be_true();
		FakeCard card = harness::factoryCardWithPack();
		auto picks = lanePicks(card);
		expect(picks[Lane::Riser].size()).to_be_greater_than(size_t{2});
		for (const std::string& riser : picks[Lane::Riser]) {
			expect(riser.rfind("SAMPLES/FX/Riser/GEN ", 0)).to_equal(size_t{0});
		}
	});

	it("gives every FX lane a pack synth preset for a card without samples", _ {
		FakeCard card = harness::factoryCardWithPack();
		for (const PartSpec& part : techno.parts) {
			for (const LaneSpec& lane : part.lanes) {
				if (!isFxLane(lane.lane)) {
					continue;
				}
				const LaneSounds* sounds = techno.palette->laneSounds(lane.lane);
				expect(sounds != nullptr && !sounds->synths.empty()).to_be_true();
				for (const PaletteEntry& entry : sounds->synths) {
					expect(std::string(entry.path).rfind("SYNTHS/GENERATIVE/FX/", 0)).to_equal(size_t{0});
					expect(card.has(entry.path)).to_be_true();
				}
			}
		}
	});

	it("names no tag or exact path that matches nothing on the card", _ {
		FakeCard card = harness::factoryCardWithPack();
		for (const LaneSounds& sounds : techno.palette->lanes) {
			for (const TagWeight& tag : sounds.tags) {
				// One lane entry with just this tag, rolled: it must find a file.
				TagWeight only[] = {tag};
				LaneSounds single = sounds;
				single.tags = only;
				LaneSounds lanes[] = {single};
				Palette palette{.lanes = lanes};
				LaneSpec laneSpecs[] = {{.lane = sounds.lane}};
				PartSpec parts[] = {{.role = isFxLane(sounds.lane) ? Role::FX : Role::Drums, .lanes = laneSpecs}};
				Genre genre{.parts = parts, .palette = &palette};
				Recipe recipe = newRecipe(genre, 1);
				rollSounds(recipe, 1, card);
				if (recipe.sounds.lane[static_cast<int32_t>(sounds.lane)].empty()) {
					std::printf("no file for tag \"%s\"\n", tag.tag);
				}
				expect(recipe.sounds.lane[static_cast<int32_t>(sounds.lane)].empty()).to_be_false();
			}
		}
	});
});

CPPSPEC_SPEC(techno_palette)
