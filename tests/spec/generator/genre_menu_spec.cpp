// The rows of GENRE ▸ (spec §14.1, §13.6): a parent genre is one row that opens a submenu of its subgenres, and the
// menu opens on a genre's row, and in its parent's submenu on the genre itself.

#include "generative/perform/genre_menu.h"

#include "cppspec.hpp"

#include <array>
#include <string>

using namespace deluge::generative;
using namespace deluge::generative::perform;

namespace {

constexpr Genre kHouse{.name = "house", .oledName = "HOUSE"};
constexpr Genre kTechno{.name = "techno", .oledName = "TECHNO", .version = 0};
constexpr Genre kAcid = [] {
	Genre genre = subgenreOf(kTechno);
	genre.name = "techno-acid";
	genre.shortName = "ACID";
	return genre;
}();
constexpr Genre kDub = [] {
	Genre genre = subgenreOf(kTechno);
	genre.name = "techno-dub";
	genre.shortName = "DUB";
	return genre;
}();
constexpr Genre kAmbient{.name = "ambient", .oledName = "AMBIENT"};

/// Techno's subgenres apart in the list: the group still sits where the first one is.
constexpr std::array<const Genre*, 4> kGenres = {&kAcid, &kHouse, &kDub, &kAmbient};

} // namespace

// clang-format off
describe genre_menu("GENRE ▸", $ {
	it("has one row per genre with no parent, and one per parent where its first subgenre is", _ {
		expect(genreRow(kGenres, 0).genre == &kTechno).to_be_true();
		expect(genreRow(kGenres, 0).group).to_be_true();
		expect(genreRow(kGenres, 1).genre == &kHouse).to_be_true();
		expect(genreRow(kGenres, 1).group).to_be_false();
		expect(genreRow(kGenres, 2).genre == &kAmbient).to_be_true();
		expect(genreRow(kGenres, 3).genre == nullptr).to_be_true();
		expect(genreRow(kGenres, -1).genre == nullptr).to_be_true();
	});

	it("lists a parent's subgenres in genre order in its submenu", _ {
		expect(subgenreAt(kGenres, kTechno, 0) == &kAcid).to_be_true();
		expect(subgenreAt(kGenres, kTechno, 1) == &kDub).to_be_true();
		expect(subgenreAt(kGenres, kTechno, 2) == nullptr).to_be_true();
		expect(subgenreAt(kGenres, kHouse, 0) == nullptr).to_be_true();
	});

	it("opens on a genre's row, and on a subgenre in its parent's submenu", _ {
		expect(genreSpot(kGenres, kHouse).row).to_equal(1);
		expect(genreSpot(kGenres, kHouse).inGroup).to_equal(-1);
		expect(genreSpot(kGenres, kDub).row).to_equal(0);
		expect(genreSpot(kGenres, kDub).inGroup).to_equal(1);
		expect(genreSpot(kGenres, kAmbient).row).to_equal(2);
	});

	it("puts the built-in Techno subgenres in one TECHNO row, opening on the default genre", _ {
		const GenreRow first = genreRow(allGenres(), 0);
		expect(first.group).to_be_true();
		expect(std::string(first.genre->oledName)).to_equal(std::string("TECHNO"));
		expect(genreRow(allGenres(), 1).genre == nullptr).to_be_true();
		const GenreSpot spot = genreSpot(allGenres(), defaultGenre());
		expect(spot.row).to_equal(0);
		expect(subgenreAt(allGenres(), *first.genre, spot.inGroup) == &defaultGenre()).to_be_true();
	});
});

CPPSPEC_SPEC(genre_menu)
