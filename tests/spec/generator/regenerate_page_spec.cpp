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

// The Regenerate and genre-change page (spec §13.6, ADR 0008 "The menus"): what it opens on, what its VARIANTS rows
// show and edit, what GO commits and how a Regenerate draws within it; and the OLED names (set and variant names, the
// page's titles) against the menu fonts' metrics.

#include "generative/core/genre.h"
#include "generative/core/recipe.h"
#include "generative/core/variants.h"
#include "generative/perform/genre_menu.h"
#include "generative/perform/menu_rows.h"
#include "generative/perform/regenerate_page.h"
#include "gui/fonts/fonts.h"
#include "no_sets_genre.h"

#include "cppspec.hpp"

#include <cstring>
#include <string>
#include <string_view>

using namespace deluge::generative;
using namespace deluge::generative::perform;

namespace {

constexpr Variant kThree[] = {
    {.name = "a", .oledName = "A"}, {.name = "b", .oledName = "B"}, {.name = "c", .oledName = "CEE"}};
constexpr VariantSet kSet{.name = "set", .oledName = "SET", .variants = kThree};

/// The title font (Canvas::drawScreenTitle(): text height 10, the bold 9 px Metric font): a string's width as
/// Canvas::getStringWidthInPixels() takes it, without its kerning, which only ever narrows a pair. Lowercase draws as
/// uppercase, as getCharIndex() maps it.
int32_t titlePixels(std::string_view text) {
	int32_t width = 0;
	for (size_t i = 0; i < text.size(); i++) {
		uint8_t c = static_cast<uint8_t>(text[i]);
		if (c >= 'a' && c <= 'z') {
			c = static_cast<uint8_t>(c - 32);
		}
		const int32_t index = c - 0x20;
		width += index > 0 ? font_metric_bold_9px_desc[index].w_px : 0; // a space has no glyph
		if (i + 1 < text.size()) {
			width += c == ' ' ? 5 : 2; // Canvas::getCharSpacingInPixels() at height 10
		}
	}
	return width;
}

/// The longest text a set's VARIANTS row shows as its value: RANDOM, `n OF m` or a variant's OLED name.
int32_t longestValue(const VariantSet& set) {
	int32_t longest = static_cast<int32_t>(std::string_view("RANDOM").size());
	longest = std::max(longest, static_cast<int32_t>(std::string_view("8 OF 8").size()));
	for (const Variant& variant : set.variants) {
		longest = std::max(longest, static_cast<int32_t>(std::string_view(variant.oledName).size()));
	}
	return longest;
}

} // namespace

// clang-format off
describe regenerate_page("The Regenerate and genre-change page", $ {
	context("a set's row", _ {
		it("shows RANDOM with every variant allowed, the variant with one, and n OF m for a subset", _ {
			expect(allowedShown(kSet, 0b111).kind == AllowedShown::Kind::Random).to_be_true();
			const AllowedShown one = allowedShown(kSet, 0b100);
			expect(one.kind == AllowedShown::Kind::One).to_be_true();
			expect(static_cast<int>(one.variant)).to_equal(2);
			const AllowedShown subset = allowedShown(kSet, 0b101);
			expect(subset.kind == AllowedShown::Kind::Subset).to_be_true();
			expect(static_cast<int>(subset.count)).to_equal(2);
			expect(static_cast<int>(subset.of)).to_equal(3);
		});

		it("reads bits past the set's variants, and none, as the draw does (allowedIn)", _ {
			expect(allowedShown(kSet, 0b1111'1111).kind == AllowedShown::Kind::Random).to_be_true();
			expect(allowedShown(kSet, 0).kind == AllowedShown::Kind::Random).to_be_true();
		});

		it("turns a tick over, but keeps the last one", _ {
			expect(static_cast<int>(toggledAllowed(kSet, 0b111, 1))).to_equal(0b101);
			expect(static_cast<int>(toggledAllowed(kSet, 0b101, 1))).to_equal(0b111);
			expect(static_cast<int>(toggledAllowed(kSet, 0b100, 2))).to_equal(0b100);
			expect(static_cast<int>(toggledAllowed(kSet, 0b100, 0))).to_equal(0b101);
		});

		it("ignores a variant past the set's", _ {
			expect(static_cast<int>(toggledAllowed(kSet, 0b011, 5))).to_equal(0b011);
		});
	});

	context("a Recipe's picks in test events", _ {
		it("name each set's pick and allowed mask, or - for a genre with no sets", _ {
			VariantPicks picks = allRandom(genres::dub);
			fixVariant(picks, parseVariantFix(genres::dub, "skank=plain").value());
			char text[kPicksTextChars];
			picksText(genres::dub, picks, text);
			expect(std::string(text)).to_equal(std::string("skank=plain:2 drums=onedrop:7 break=seven:3 mix=version:7 bass=bursts:7"));
			picksText(harness::genreWithNoSets(), VariantPicks{}, text);
			expect(std::string(text)).to_equal(std::string("-"));
		});

		it("fit every built-in genre's longest picks text in kPicksTextChars, within 120 chars for the test port's line", _ {
			// The test port's 192-char event line holds a timestamp and the event's fields before the picks.
			for (const Genre* genre : allGenres()) {
				VariantPicks picks = allRandom(*genre);
				for (size_t s = 0; s < genre->variantSets.size(); s++) {
					const VariantSet& set = genre->variantSets[s];
					for (size_t v = 0; v < set.variants.size(); v++) {
						if (std::strlen(set.variants[v].name) > std::strlen(set.variants[picks.pick[s]].name)) {
							picks.pick[s] = static_cast<uint8_t>(v);
						}
					}
				}
				char wide[512];
				picksText(*genre, picks, wide);
				expect(std::strlen(wide)).to_be_less_than(size_t{121});
				expect(std::strlen(wide)).to_be_less_than(kPicksTextChars);
			}
		});

		it("name each set's allowed mask for GO's event, or - for a genre with no sets", _ {
			VariantPicks picks = allRandom(genres::dub);
			char text[kPicksTextChars];
			masksText(genres::dub, picks, text);
			expect(std::string(text)).to_equal(std::string("skank:3 drums:7 break:3 mix:7 bass:7"));
			masksText(harness::genreWithNoSets(), VariantPicks{}, text);
			expect(std::string(text)).to_equal(std::string("-"));
		});
	});

	context("the Compose screen's VARIANTS rows on the Current Recipe", _ {
		const VariantSet& skank = genres::dub.variantSets[0];
		const auto sevenths = static_cast<uint8_t>(findVariant(skank, "sevenths"));
		const auto plain = static_cast<uint8_t>(findVariant(skank, "plain"));

		it("redraw the variant playing within the ticks when it's unticked", _ {
			for (uint32_t entropy = 0; entropy < 20; entropy++) {
				Recipe recipe = newRecipe(genres::dub, 7);
				recipe.variants.pick[0] = sevenths;
				expect(narrowSet(recipe, 0, static_cast<uint8_t>(1U << plain), entropy)).to_be_true();
				expect(static_cast<int>(recipe.variants.pick[0])).to_equal(plain);
				expect(static_cast<int>(recipe.variants.allowed[0])).to_equal(1 << plain);
			}
		});

		it("keep the variant playing while it's still ticked", _ {
			Recipe recipe = newRecipe(genres::dub, 7);
			recipe.variants.pick[0] = plain;
			const Recipe before = recipe;
			expect(narrowSet(recipe, 0, static_cast<uint8_t>(1U << plain), 3)).to_be_false();
			expect(static_cast<int>(recipe.variants.pick[0])).to_equal(plain);
			recipe.variants.allowed[0] = before.variants.allowed[0];
			expect(recipe == before).to_be_true(); // nothing but the mask moved
		});
	});

	context("opening", _ {
		it("starts on the incoming genre's transition entry with every set on RANDOM", _ {
			RegeneratePage page;
			page.open(genres::dub, false);
			expect(page.genre() == &genres::dub).to_be_true();
			expect(page.style() == confirmStyle(genres::dub)).to_be_true();
			expect(page.showsVariants()).to_be_true();
			expect(static_cast<int>(page.narrowed().allowed[0])).to_equal(0b11);
			page.open(genres::ambient, false);
			expect(page.style() == TransitionStyle::Fade).to_be_true();
		});

		it("shows no VARIANTS row for a genre with no sets, nor for a LOAD", _ {
			RegeneratePage page;
			page.open(harness::genreWithNoSets(), false);
			expect(page.showsVariants()).to_be_false();
			page.open(genres::dub, true);
			expect(page.showsVariants()).to_be_false();
			expect(page.load()).to_be_true();
		});

		it("forgets a narrowed set and a style when it opens again", _ {
			RegeneratePage page;
			page.open(genres::dub, false);
			page.setStyle(TransitionStyle::Cut);
			page.narrowed().allowed[0] = toggledAllowed(genres::dub.variantSets[0], page.narrowed().allowed[0], 0);
			page.open(genres::dub, false);
			expect(page.style() == confirmStyle(genres::dub)).to_be_true();
			expect(allowedShown(genres::dub.variantSets[0], page.narrowed().allowed[0]).kind
			       == AllowedShown::Kind::Random).to_be_true();
		});
	});

	context("GO", _ {
		it("commits the style and the narrowed masks", _ {
			RegeneratePage page;
			page.open(genres::dub, false);
			page.setStyle(TransitionStyle::Fade);
			page.narrowed().allowed[0] = toggledAllowed(genres::dub.variantSets[0], page.narrowed().allowed[0], 0);
			const RegeneratePage::Choice choice = page.go();
			expect(choice.style == TransitionStyle::Fade).to_be_true();
			expect(static_cast<int>(choice.narrowed.allowed[0])).to_equal(0b10);
		});

		it("commits every set on RANDOM for a LOAD", _ {
			RegeneratePage page;
			page.open(genres::dub, true);
			page.narrowed().allowed[0] = 0b01; // never edited through the page, which shows no VARIANTS
			expect(page.go().narrowed == VariantPicks{}).to_be_true();
		});

		it("has a Regenerate draw only within a narrowed set, and storing the subset", _ {
			RegeneratePage page;
			page.open(genres::dub, false);
			const int32_t plain = findVariant(genres::dub.variantSets[0], "plain");
			page.narrowed().allowed[0] = static_cast<uint8_t>(1U << plain);
			const RegeneratePage::Choice choice = page.go();
			for (uint32_t entropy = 0; entropy < 200; entropy++) {
				const Recipe recipe = newRecipe(genres::dub, entropy, std::nullopt, &choice.narrowed);
				expect(static_cast<int>(recipe.variants.pick[0])).to_equal(plain);
				expect(static_cast<int>(recipe.variants.allowed[0])).to_equal(1 << plain);
			}
		});

		it("leaves an all-RANDOM Regenerate drawing as a Regenerate with no page did", _ {
			RegeneratePage page;
			page.open(genres::dub, false);
			const RegeneratePage::Choice choice = page.go();
			for (uint32_t entropy = 0; entropy < 50; entropy++) {
				expect(newRecipe(genres::dub, entropy, std::nullopt, &choice.narrowed)
				       == newRecipe(genres::dub, entropy)).to_be_true();
			}
		});
	});

	context("the OLED names", _ {
		it("fit each set's VARIANTS row (name, value and arrow) and each variant's multi-select row, in the menu font", _ {
			for (const Genre* genre : allGenres()) {
				for (const VariantSet& set : genre->variantSets) {
					const auto nameChars = static_cast<int32_t>(std::string_view(set.oledName).size());
					expect(menuRowFits(nameChars, longestValue(set), true)).to_be_true();
					for (const Variant& variant : set.variants) {
						const auto chars = static_cast<int32_t>(std::string_view(variant.oledName).size());
						expect(menuRowFits(chars, 0, true)).to_be_true();
					}
					expect(titlePixels(set.oledName)).to_be_less_than(kMenuWidthPixels + 1);
				}
			}
		});

		it("are as long as the menu font lets the longest names be (ADR 0008: 8 and 9 characters)", _ {
			expect(menuRowFits(kMaxVariantSetOledName, kMaxVariantOledName, true)).to_be_true();
			expect(menuRowFits(kMaxVariantSetOledName + 1, kMaxVariantOledName, true)).to_be_false();
			expect(menuRowFits(kMaxVariantSetOledName, kMaxVariantOledName + 1, true)).to_be_false();
		});

		it("fit the page's title, `<GENRE>?`, for every genre, in the title font", _ {
			for (const Genre* genre : allGenres()) {
				const std::string title = std::string(genre->oledName) + "?";
				expect(titlePixels(title)).to_be_less_than(kMenuWidthPixels + 1);
			}
			expect(titlePixels("REGENERATE")).to_be_less_than(kMenuWidthPixels + 1);
			expect(titlePixels("VARIANTS")).to_be_less_than(kMenuWidthPixels + 1);
		});

		it("measures the title font as the Canvas does", _ {
			expect(titlePixels("I")).to_equal(2);
			expect(titlePixels("II")).to_equal(6);
			expect(titlePixels("I I")).to_equal(11); // a space: no glyph, 5 px after it
		});
	});
});

CPPSPEC_SPEC(regenerate_page);
