// Subgenres (spec §4.1, §4.4, ADR 0006): a genre may name a parent genre, a table that is never played, and be written
// as overrides of it. It inherits every field it doesn't list, but not the parent's identity.

#include "generative/core/genre.h"

#include "cppspec.hpp"

#include <cstring>
#include <string>

using namespace deluge::generative;

namespace {

constexpr Pattern kTestKick[] = {{"X...X...X...X...", "................", 0}};
constexpr LaneSpec kTestLanes[] = {{.lane = Lane::Kick, .pool = kTestKick, .band = {0, 0}}};
constexpr PartSpec kTestParts[] = {{.role = Role::Drums, .name = "DRUMS", .lanes = kTestLanes}};
constexpr Scale kTestScales[] = {Scale::Dorian};
constexpr FillEntry kTestFills[] = {
    {.kind = FillKind::Rest, .mask = {.lanes = laneBit(Lane::Kick)}, .beats = 1, .weight = 1}};

/// A test-only parent, with a value off the Generator-wide default in each kind of field.
constexpr Genre kTestParent{
    .name = "polka",
    .oledName = "POLKA",
    .version = 0,
    .facts = {.tempoLowBpm = 101, .tempoHighBpm = 103, .scales = kTestScales, .swing = 7},
    .throwHoldSteps = 3,
    .phraseBars = 4,
    .parts = kTestParts,
    .densityJitter = 41,
    .repickChance = 77,
    .fills = kTestFills,
    .fillChance = 123,
    .harmony = {.extensionFloor = 1},
    .palette = &genres::technoPalette,
    .arc = {.introPhrases = 3},
};

/// A test-only subgenre that changes two fields.
constexpr Genre kTestChild = [] {
	Genre genre = subgenreOf(kTestParent);
	genre.name = "polka-fast";
	genre.oledName = "FAST POLKA";
	genre.shortName = "FAST";
	genre.facts.tempoLowBpm = 140;
	genre.phraseBars = 8;
	return genre;
}();

bool startsWith(const char* text, const std::string& prefix) {
	return std::strncmp(text, prefix.c_str(), prefix.size()) == 0;
}

} // namespace

// clang-format off
describe subgenre("Subgenres", $ {
	context("A subgenre", _ {
		it("names its parent and changes only the fields it lists", _ {
			expect(kTestChild.parent == &kTestParent).to_be_true();
			expect(kTestChild.facts.tempoLowBpm).to_equal(140);
			expect(kTestChild.phraseBars).to_equal(8);
		});

		it("reads every field it doesn't list from its parent", _ {
			expect(kTestChild.facts.tempoHighBpm).to_equal(103);
			expect(kTestChild.facts.scales.data() == kTestScales).to_be_true();
			expect(kTestChild.facts.swing).to_equal(7);
			expect(kTestChild.throwHoldSteps).to_equal(3);
			expect(kTestChild.parts.data() == kTestParts).to_be_true();
			expect(kTestChild.densityJitter).to_equal(41);
			expect(kTestChild.repickChance).to_equal(77);
			expect(kTestChild.fills.data() == kTestFills).to_be_true();
			expect(kTestChild.fillChance).to_equal(123);
			expect(kTestChild.harmony.extensionFloor).to_equal(1);
			expect(kTestChild.palette == &genres::technoPalette).to_be_true();
			expect(kTestChild.arc.introPhrases).to_equal(3);
		});

		it("starts from no identity of the parent's: its names and version are its own", _ {
			constexpr Genre bare = subgenreOf(kTestParent);
			expect(std::strlen(bare.name)).to_equal(size_t{0});
			expect(std::strlen(bare.oledName)).to_equal(size_t{0});
			expect(std::strlen(bare.shortName)).to_equal(size_t{0});
			expect(bare.version).to_equal(1);
		});
	});

	context("Every built-in subgenre", _ {
		it("has a parent with no parent of its own, which is never played", _ {
			for (const Genre* genre : allGenres()) {
				if (genre->parent == nullptr) {
					continue;
				}
				expect(genre->parent->parent == nullptr).to_be_true();
				expect(findGenre(genre->parent->name) != genre->parent).to_be_true();
				for (const Genre* other : allGenres()) {
					expect(other != genre->parent).to_be_true();
				}
				// A parent's table carries no version (spec §4.4).
				expect(genre->parent->version).to_equal(0);
			}
		});

		it("has the stable name <parent>-<own>, an OLED name and a short name for its row (spec §4.4)", _ {
			for (const Genre* genre : allGenres()) {
				if (genre->parent == nullptr) {
					continue;
				}
				expect(std::strlen(genre->oledName)).to_be_greater_than(size_t{0});
				expect(std::strlen(genre->shortName)).to_be_greater_than(size_t{0});
				expect(std::strlen(genre->shortName)).to_be_less_than(std::strlen(genre->oledName) + 1);
				if (genre == &genres::techno) {
					continue; // today's Techno keeps its name until #218 renames it techno-acid
				}
				const std::string prefix = std::string(genre->parent->name) + "-";
				expect(startsWith(genre->name, prefix)).to_be_true();
				expect(std::strlen(genre->name)).to_be_greater_than(prefix.size());
			}
		});

		it("puts Techno under the Techno parent, as the default genre (spec §4.5)", _ {
			expect(genres::techno.parent != nullptr).to_be_true();
			expect(std::string(genres::techno.parent->name)).to_equal(std::string("techno"));
			expect(std::string(genres::techno.parent->oledName)).to_equal(std::string("TECHNO"));
			expect(&defaultGenre() == &genres::techno).to_be_true();
		});

		it("puts Melodic techno beside it, with a roster of another size, and no stand-in (#226)", _ {
			const Genre* melodic = findGenre("techno-melodic");
			expect(melodic == &genres::technoMelodic).to_be_true();
			expect(melodic->parent == genres::techno.parent).to_be_true();
			expect(std::string(melodic->oledName)).to_equal(std::string("MELODIC TECHNO"));
			expect(std::string(melodic->shortName)).to_equal(std::string("MELODIC"));
			expect(partCount(*melodic) != partCount(genres::techno)).to_be_true();
			expect(findGenre("techno-standin") == nullptr).to_be_true();
		});

		it("fits the built-in genre cap of eight (ADR 0006)", _ {
			expect(kMaxGenres).to_equal(8);
			expect(static_cast<int32_t>(allGenres().size())).to_be_less_than(kMaxGenres + 1);
			expect(allGenres().size()).to_be_greater_than(size_t{1});
		});
	});
});

CPPSPEC_SPEC(subgenre)
