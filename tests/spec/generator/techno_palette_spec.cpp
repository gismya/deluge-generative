// Techno's built-in palette against the factory card plus the Generative pack (spec §10.1–§10.5, §18.3).

#include "fake_card.h"
#include "generative/core/genre.h"
#include "generative/core/palette.h"
#include "generative/core/recipe.h"
#include "palette_helpers.h"

#include "cppspec.hpp"

#include <map>
#include <set>
#include <string>

using namespace deluge::generative;
using harness::FakeCard;

namespace {

const Genre& techno = genres::technoAcid;

} // namespace

// clang-format off
describe techno_palette("Acid techno's palette", $ {
	it("gives every synth part a preset at least, each on the factory card or in the pack", _ {
		// ACID keeps 005 Sweet Mono Bass alone after the ear check (spec §10.1, #120 round 1).
		FakeCard card = harness::factoryCardWithPack();
		expect(techno.palette != nullptr).to_be_true();
		expect(harness::fewestPartPresets(techno)).to_be_greater_than(size_t{0});
		expect(harness::partPresetsMissing(techno, card).empty()).to_be_true();
	});

	it("gives every lane Techno plays at least 3 files after tag filtering, its kit aside, and the Impact its 2 presets",
	   _ {
		   FakeCard card = harness::factoryCardWithPack();
		   auto picks = harness::lanePicks(techno, card, 800, {}, false);
		   for (const PartSpec& part : techno.parts) {
			   for (const LaneSpec& lane : part.lanes) {
				   if (lane.lane != Lane::Impact) {
					   expect(picks[lane.lane].size()).to_be_greater_than(size_t{2});
				   }
			   }
		   }
		   // The owner's picks (#472): the boom or the wash, a piece each (spec §10.5); Acid's own boom, 1.6 dB under the
		   // others', so its Drop's mix doesn't clip (impact round 3).
		   expect(picks[Lane::Impact].size()).to_equal(size_t{2});
		   expect(picks[Lane::Impact].contains("SYNTHS/GENERATIVE/FX/GEN Impact Boom Acid.XML")).to_be_true();
		   expect(picks[Lane::Impact].contains("SYNTHS/GENERATIVE/FX/GEN Impact Wash.XML")).to_be_true();
	   });

	it("plays an FX part whose Riser picks only pack risers", _ {
		bool hasFx = false;
		for (const PartSpec& part : techno.parts) {
			hasFx = hasFx || part.role == Role::FX;
		}
		expect(hasFx).to_be_true();
		FakeCard card = harness::factoryCardWithPack();
		auto picks = harness::lanePicks(techno, card, 800);
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
				SoundPicks picks;
				rollSounds(genre, palette, picks, 1, card);
				if (picks.lane[static_cast<int32_t>(sounds.lane)].empty()) {
					std::printf("no file for tag \"%s\"\n", tag.tag);
				}
				expect(picks.lane[static_cast<int32_t>(sounds.lane)].empty()).to_be_false();
			}
		}
	});
});

CPPSPEC_SPEC(techno_palette)
