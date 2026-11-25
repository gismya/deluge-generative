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

// Techno's variant sets (variant ideas 1, #531, ADR 0008): DRUMS on Acid and Detroit techno, ACID, Melodic techno's
// ARP, and the parent's BUILD on every subgenre. Their names, weights and shares, and what each variant plays.

#include "core_walk.h"
#include "generative/core/variants.h"
#include "harness.h"
#include "stream_helpers.h"

#include "cppspec.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace harness;

namespace {

constexpr int32_t kDrumsPart = 0;
constexpr int32_t kBassPart = 1;
constexpr int32_t kArpPart = 3; // Melodic techno's

/// Each subgenre's sets: their stable and OLED names, then each variant's stable name.
std::vector<std::vector<std::string>> setNames(const Genre& genre) {
	std::vector<std::vector<std::string>> out;
	for (const VariantSet& set : genre.variantSets) {
		out.push_back({set.name, set.oledName});
		for (const Variant& variant : set.variants) {
			out.back().emplace_back(variant.name);
		}
	}
	return out;
}

/// `genre`'s Recipe from `entropy`, Motion high enough to reach Builds and Drops, every set as written but `fixes`.
Recipe with(const Genre& genre, std::initializer_list<const char*> fixes, uint32_t entropy, int32_t density = 25) {
	return withVariants(writtenVariants(movingRecipe(genre, entropy, density)), fixes);
}

/// Note-ons of `part` (on `lane`, any for Lane::None) in the Groove's steps of `walked`, unmuted, by step.
std::vector<Event> grooveOns(const Walked& walked, int32_t part, Lane lane = Lane::None) {
	std::vector<Event> out;
	for (const Event& e : walked.noteOns) {
		const int32_t step = std::min<int32_t>(stepOf(e), static_cast<int32_t>(walked.steps.size()) - 1);
		if (e.part == part && (lane == Lane::None || e.lane == lane) && !e.muted
		    && walked.steps[step].section == SectionKind::Groove) {
			out.push_back(e);
		}
	}
	return out;
}

/// The Groove's bars in `walked`.
int32_t grooveBars(const Walked& walked) {
	int32_t steps = 0;
	for (const Walked::Step& step : walked.steps) {
		steps += step.section == SectionKind::Groove ? 1 : 0;
	}
	return steps / kStepsPerBar;
}

/// Hits a Groove bar of `part` on `lane` in `genre` with `fixes`, over seeds 1–4.
double perGrooveBar(const Genre& genre, std::initializer_list<const char*> fixes, int32_t part, Lane lane = Lane::None,
                    int32_t density = 25) {
	int32_t hits = 0;
	int32_t bars = 0;
	for (uint32_t seed = 1; seed <= 4; seed++) {
		const Walked walked = walk(with(genre, fixes, seed, density), seed, 96 * kStepsPerBar, {}, true);
		hits += static_cast<int32_t>(grooveOns(walked, part, lane).size());
		bars += grooveBars(walked);
	}
	return bars > 0 ? static_cast<double>(hits) / bars : 0;
}

/// The lengths, in bars, of every whole Build of `genre` with `fixes` over seeds 1–6.
std::set<int32_t> buildLengths(const Genre& genre, const char* fix) {
	std::set<int32_t> out;
	for (uint32_t seed = 1; seed <= 6; seed++) {
		const Walked walked = walk(with(genre, {fix}, seed), seed, 200 * kStepsPerBar);
		int32_t run = 0;
		for (const Walked::Step& step : walked.steps) {
			if (step.section == SectionKind::Build) {
				run++;
			}
			else if (run > 0) {
				out.insert(run / kStepsPerBar);
				run = 0;
			}
		}
	}
	return out;
}

/// The steps of each Build's last beat, its last bar, the bar before, and its whole, in `walked`.
struct BuildSteps {
	std::set<int32_t> lastBeat;
	std::set<int32_t> lastBar;
	std::set<int32_t> barBefore;
	std::set<int32_t> all;
};
BuildSteps buildSteps(const Walked& walked) {
	BuildSteps out;
	for (size_t n = 0; n + 1 < walked.steps.size(); n++) {
		if (walked.steps[n].section != SectionKind::Build) {
			continue;
		}
		out.all.insert(static_cast<int32_t>(n));
		if (walked.steps[n + 1].drop) {
			for (int32_t k = 0; k < kStepsPerBar; k++) {
				if (k < kStepsPerBeat) {
					out.lastBeat.insert(static_cast<int32_t>(n) - k);
				}
				out.lastBar.insert(static_cast<int32_t>(n) - k);
				out.barBefore.insert(static_cast<int32_t>(n) - kStepsPerBar - k);
			}
		}
	}
	return out;
}

} // namespace

// clang-format off
describe techno_variants("Techno's variant sets", $ {
	context("the data (variant ideas 1, ADR 0008)", _ {
		it("lists DRUMS, ACID and BUILD on Acid techno, DRUMS and BUILD on Detroit, ARP and BUILD on Melodic, BUILD on Dub", _ {
			const std::vector<std::string> drums{"drums", "DRUMS", "offbeat", "running", "pump", "tribal"};
			const std::vector<std::string> build{"build", "BUILD", "roll", "long", "noroll", "hard"};
			expect(setNames(genres::technoAcid) == std::vector<std::vector<std::string>>{
			    drums, {"acid", "ACID", "classic", "talking", "hypnotic"}, build}).to_be_true();
			expect(setNames(genres::technoDetroit) == std::vector<std::vector<std::string>>{drums, build}).to_be_true();
			expect(setNames(genres::technoMelodic) == std::vector<std::vector<std::string>>{
			    {"arp", "ARP", "rising", "pulse", "down", "sparse"}, build}).to_be_true();
			expect(setNames(genres::technoDub) == std::vector<std::vector<std::string>>{build}).to_be_true();
		});

		it("weights them as the owner marked them, the genre as written first, the Build not live and the rest live", _ {
			auto weights = [](const VariantSet& set) {
				std::vector<int32_t> out;
				for (const Variant& variant : set.variants) {
					out.push_back(variant.weight);
				}
				return out;
			};
			for (const Genre* genre : {&genres::technoAcid, &genres::technoDetroit, &genres::technoMelodic, &genres::technoDub}) {
				for (const VariantSet& set : genre->variantSets) {
					expect(set.variants[0].apply == nullptr).to_be_true();
					expect(set.changeAt == ChangeAt::Never).to_be_true();
					expect(set.live).to_equal(std::string(set.name) != "build");
				}
				// The parent's BUILD, one table for every subgenre.
				const VariantSet& build = genre->variantSets.back();
				expect(build.variants.data() == genres::technoDub.variantSets[0].variants.data()).to_be_true();
				expect(weights(build)).to_equal(std::vector<int32_t>{3, 1, 2, 2});
			}
			expect(weights(genres::technoAcid.variantSets[0])).to_equal(std::vector<int32_t>{2, 2, 2, 1});
			expect(weights(genres::technoDetroit.variantSets[0])).to_equal(std::vector<int32_t>{2, 2, 2, 1});
			expect(weights(genres::technoAcid.variantSets[1])).to_equal(std::vector<int32_t>{3, 2, 2});
			expect(weights(genres::technoMelodic.variantSets[0])).to_equal(std::vector<int32_t>{3, 3, 2, 2});
		});

		it("lands each variant at its weight's share of 1,000 new Recipes, within 5 points", _ {
			for (const Genre* genre : {&genres::technoAcid, &genres::technoDetroit, &genres::technoMelodic, &genres::technoDub}) {
				const auto sets = static_cast<int32_t>(genre->variantSets.size());
				std::vector<std::vector<int32_t>> counts(sets, std::vector<int32_t>(kMaxVariants, 0));
				for (uint32_t entropy = 0; entropy < 1000; entropy++) {
					const Recipe recipe = newRecipe(*genre, entropy * 2654435761U);
					for (int32_t s = 0; s < sets; s++) {
						counts[s][recipe.variants.pick[s]]++;
					}
				}
				for (int32_t s = 0; s < sets; s++) {
					const VariantSet& set = genre->variantSets[s];
					int32_t total = 0;
					for (const Variant& variant : set.variants) {
						total += variant.weight;
					}
					for (size_t v = 0; v < set.variants.size(); v++) {
						const int32_t expected = 1000 * set.variants[v].weight / total;
						expect(std::abs(counts[s][v] - expected)).to_be_less_than(51);
					}
				}
			}
		});
	});

	context("DRUMS, the hat style (Acid and Detroit techno)", _ {
		it("runs the closed hat on 16ths with no Lock, and the open hat mostly out at the default Density", _ {
			for (const Genre* genre : {&genres::technoAcid, &genres::technoDetroit}) {
				expect(perGrooveBar(*genre, {"drums=offbeat"}, kDrumsPart, Lane::ClosedHat)).to_be_less_than(8.0);
				expect(perGrooveBar(*genre, {"drums=running"}, kDrumsPart, Lane::ClosedHat)).to_be_greater_than(12.0);
				// Importance 5 or 6: Density's jitter lets it through in a pick now and then (about 0.5 a bar, where the
				// offbeat hats play 2).
				expect(perGrooveBar(*genre, {"drums=running"}, kDrumsPart, Lane::OpenHat))
				    .to_be_less_than(0.4 * perGrooveBar(*genre, {"drums=offbeat"}, kDrumsPart, Lane::OpenHat));
				ResolvedGenre copy;
				const Genre& running = resolveGenre(*genre, with(*genre, {"drums=running"}, 0).variants, &copy);
				expect(laneOf(running, kDrumsPart, Lane::ClosedHat).rules.empty()).to_be_true();
				expect(laneOf(*genre, kDrumsPart, Lane::ClosedHat).rules.empty()).to_be_false();
			}
		});

		it("pumps: the kick only on the beats, the open hat on every &", _ {
			for (const Genre* genre : {&genres::technoAcid, &genres::technoDetroit}) {
				std::map<int32_t, int32_t> kicksBySixteenth;
				int32_t openOnAnd = 0;
				int32_t bars = 0;
				for (uint32_t seed = 1; seed <= 4; seed++) {
					const Walked walked =
					    walk(with(*genre, {"drums=pump"}, seed, 40), seed, 96 * kStepsPerBar, {}, true);
					for (const Event& e : grooveOns(walked, kDrumsPart, Lane::Kick)) {
						kicksBySixteenth[stepOf(e) % kStepsPerBeat]++;
					}
					for (const Event& e : grooveOns(walked, kDrumsPart, Lane::OpenHat)) {
						openOnAnd += stepOf(e) % kStepsPerBeat == 2 ? 1 : 0;
					}
					bars += grooveBars(walked);
				}
				// The only kicks off the beat are a fill's: none in a Groove of this length.
				expect(kicksBySixteenth[0]).to_be_greater_than(0);
				expect(kicksBySixteenth[1] + kicksBySixteenth[2] + kicksBySixteenth[3]).to_equal(0);
				expect(static_cast<double>(openOnAnd) / bars).to_be_greater_than(3.5);
			}
		});

		it("plays toms in a tribal Groove, where the others play them only in a fill", _ {
			for (const Genre* genre : {&genres::technoAcid, &genres::technoDetroit}) {
				expect(perGrooveBar(*genre, {"drums=offbeat"}, kDrumsPart, Lane::Tom)).to_be_less_than(0.5);
				expect(perGrooveBar(*genre, {"drums=tribal"}, kDrumsPart, Lane::Tom)).to_be_greater_than(1.5);
				expect(perGrooveBar(*genre, {"drums=tribal"}, kDrumsPart, Lane::Rim))
				    .to_be_greater_than(perGrooveBar(*genre, {"drums=offbeat"}, kDrumsPart, Lane::Rim) + 1.0);
			}
		});
	});

	context("ACID, how the 303 talks (Acid techno)", _ {
		it("talks more: half as many notes again a Groove bar, and the Phrygian ♭2 the line as written never plays", _ {
			expect(perGrooveBar(genres::technoAcid, {"acid=talking"}, kBassPart))
			    .to_be_greater_than(1.5 * perGrooveBar(genres::technoAcid, {"acid=classic"}, kBassPart));
			auto plays2nd = [&](const char* fix) {
				bool any = false;
				for (uint32_t seed = 1; seed <= 4; seed++) {
					Recipe recipe = with(genres::technoAcid, {fix}, seed);
					recipe.scale = Scale::Phrygian;
					const Walked walked = walk(recipe, seed, 64 * kStepsPerBar, {}, true);
					expect(walked.keys.size()).to_be_less_than(size_t{2}); // the key holds: one KeyScale at most
					for (const Event& e : walked.noteOns) {
						any = any || (e.part == kBassPart && pitchClass(e.pitch - recipe.keyRoot) == 1);
					}
				}
				return any;
			};
			expect(plays2nd("acid=talking")).to_be_true();
			expect(plays2nd("acid=classic")).to_be_false();
		});

		it("hypnotises: the root and its octaves alone", _ {
			for (uint32_t seed = 1; seed <= 4; seed++) {
				const Recipe recipe = with(genres::technoAcid, {"acid=hypnotic"}, seed);
				const Walked walked = walk(recipe, seed, 64 * kStepsPerBar, {}, true);
				std::set<int32_t> classes;
				for (const Event& e : walked.noteOns) {
					if (e.part == kBassPart) {
						classes.insert(pitchClass(e.pitch - recipe.keyRoot));
					}
				}
				expect(classes == std::set<int32_t>{0}).to_be_true();
			}
		});
	});

	context("ARP, the Arp's figure (Melodic techno)", _ {
		/// The Arp's note-ons over seeds 1–4 at Density 40, by bar.
		auto arpBars = [](const char* fix) {
			std::vector<std::vector<Event>> bars;
			for (uint32_t seed = 1; seed <= 4; seed++) {
				const Walked walked = walk(with(genres::technoMelodic, {fix}, seed, 40), seed, 64 * kStepsPerBar, {}, true);
				std::map<int32_t, std::vector<Event>> byBar;
				for (const Event& e : grooveOns(walked, kArpPart)) {
					byBar[stepOf(e) / kStepsPerBar].push_back(e);
				}
				for (auto& [bar, notes] : byBar) {
					bars.push_back(notes);
				}
			}
			return bars;
		};

		it("pulses: one pitch class a bar, the root and its octave in turn", _ {
			const auto bars = arpBars("arp=pulse");
			expect(bars.size()).to_be_greater_than(size_t{20});
			for (const std::vector<Event>& notes : bars) {
				std::set<int32_t> classes;
				std::set<int32_t> pitches;
				for (const Event& e : notes) {
					classes.insert(e.pitch % 12);
					pitches.insert(e.pitch);
				}
				expect(classes.size()).to_equal(size_t{1});
				expect(pitches.size() <= 2 && *pitches.rbegin() - *pitches.begin() <= 12).to_be_true();
			}
		});

		it("falls with down, where the walks as written climb", _ {
			auto rises = [&](const char* fix) {
				int32_t up = 0;
				int32_t down = 0;
				for (const std::vector<Event>& notes : arpBars(fix)) {
					for (size_t i = 1; i < notes.size(); i++) {
						up += notes[i].pitch > notes[i - 1].pitch ? 1 : 0;
						down += notes[i].pitch < notes[i - 1].pitch ? 1 : 0;
					}
				}
				return up - down;
			};
			expect(rises("arp=rising")).to_be_greater_than(0);
			expect(rises("arp=down")).to_be_less_than(0);
		});

		it("thins with sparse: notes on its dotted-8th grid only, fewer than the walks as written play", _ {
			// Steps no sparse pattern plays: its onsets fall a dotted 8th apart (0, 3, 6, 9, 12), with a few pickups.
			const std::set<int32_t> offGrid{1, 4, 5, 7, 11, 13};
			auto offGridNotes = [&](const char* fix) {
				int32_t count = 0;
				for (const std::vector<Event>& bar : arpBars(fix)) {
					for (const Event& e : bar) {
						count += offGrid.contains(stepOf(e) % kStepsPerBar) ? 1 : 0;
					}
				}
				return count;
			};
			expect(offGridNotes("arp=sparse")).to_equal(0);
			expect(offGridNotes("arp=rising")).to_be_greater_than(0);
			auto perBar = [&](const char* fix) {
				const auto bars = arpBars(fix);
				size_t notes = 0;
				for (const std::vector<Event>& bar : bars) {
					notes += bar.size();
				}
				return static_cast<double>(notes) / static_cast<double>(bars.size());
			};
			expect(perBar("arp=sparse")).to_be_less_than(0.8 * perBar("arp=rising"));
		});
	});

	context("BUILD, the Build's shape (every subgenre)", _ {
		it("draws the roll Build 8 or 16 bars, long 16 or 32, no roll 8, and hard 4 or 8, in every subgenre", _ {
			for (const Genre* genre : {&genres::technoAcid, &genres::technoMelodic, &genres::technoDub, &genres::technoDetroit}) {
				expect(buildLengths(*genre, "build=roll") == std::set<int32_t>{8, 16}).to_be_true();
				expect(buildLengths(*genre, "build=long") == std::set<int32_t>{16, 32}).to_be_true();
				expect(buildLengths(*genre, "build=noroll") == std::set<int32_t>{8}).to_be_true();
				expect(buildLengths(*genre, "build=hard") == std::set<int32_t>{4, 8}).to_be_true();
			}
		});

		it("rolls the snare in every Build but no roll's, leaves the Gap's beat out of hard's, and cuts long's kick for 2 bars", _ {
			for (const Genre* genre : {&genres::technoAcid, &genres::technoMelodic, &genres::technoDub, &genres::technoDetroit}) {
				struct InBuilds {
					int32_t snares = 0;
					int32_t lastBeat = 0;  ///< drum hits but the snare's in the Build's last beat
					int32_t barBefore = 0; ///< kicks in the bar before the Build's last
					int32_t lastBar = 0;   ///< kicks in the Build's last bar
				};
				auto inBuilds = [&](const char* fix) {
					InBuilds out;
					for (uint32_t seed = 1; seed <= 4; seed++) {
						const Walked walked = walk(with(*genre, {fix}, seed), seed, 200 * kStepsPerBar);
						const BuildSteps steps = buildSteps(walked);
						for (const Event& e : walked.noteOns) {
							const int32_t step = stepOf(e);
							if (e.part != kDrumsPart || e.muted) {
								continue;
							}
							out.snares += e.lane == Lane::Snare && steps.all.contains(step) ? 1 : 0;
							out.lastBeat += e.lane != Lane::Snare && steps.lastBeat.contains(step) ? 1 : 0;
							out.barBefore += e.lane == Lane::Kick && steps.barBefore.contains(step) ? 1 : 0;
							out.lastBar += e.lane == Lane::Kick && steps.lastBar.contains(step) ? 1 : 0;
						}
					}
					return out;
				};
				const InBuilds roll = inBuilds("build=roll");
				expect(roll.snares).to_be_greater_than(0);
				expect(roll.lastBeat).to_equal(0); // the Gap: a beat of silence before the Drop
				expect(roll.lastBar).to_equal(0);  // the kick cut for the last bar
				expect(inBuilds("build=noroll").snares).to_equal(0);
				expect(inBuilds("build=hard").lastBeat).to_be_greater_than(0);
				// Only Acid techno's Builds keep the kick (the others keep their Break's parts and lanes): the roll
				// Build plays it up to its last bar, and long's cuts it for its last 2.
				expect(roll.barBefore > 0).to_equal(genre == &genres::technoAcid);
				const InBuilds longBuild = inBuilds("build=long");
				expect(longBuild.lastBar + longBuild.barBefore).to_equal(0);
			}
		});
	});

	context("a live switch (spec §3.6, §13.6)", _ {
		it("lands the ACID ticks on the next Drop: the line as written before it, hypnotic's root alone after", _ {
			for (uint32_t seed = 1; seed <= 3; seed++) {
				const Recipe recipe = with(genres::technoAcid, {"acid=classic"}, seed);
				const auto acid = static_cast<uint8_t>(findVariantSet(genres::technoAcid, "acid"));
				const auto hypnotic = static_cast<uint8_t>(
				    1U << findVariant(genres::technoAcid.variantSets[acid], "hypnotic"));
				const Walked walked = walk(recipe, seed, 160 * kStepsPerBar, [&](int32_t n, const Generator&, SteeringQueue& queue) {
					if (n == kStepsPerBar) {
						queue.push({CommandKind::SetVariantMask, acid, hypnotic});
					}
				}, true);
				int32_t drop = -1;
				for (size_t n = kStepsPerBar; n < walked.steps.size() && drop < 0; n++) {
					drop = walked.steps[n].drop ? static_cast<int32_t>(n) : -1;
				}
				expect(drop).to_be_greater_than(0);
				std::set<int32_t> before;
				std::set<int32_t> after;
				for (const Event& e : walked.noteOns) {
					if (e.part == kBassPart) {
						(stepOf(e) < drop ? before : after).insert(pitchClass(e.pitch - recipe.keyRoot));
					}
				}
				expect(before.size()).to_be_greater_than(size_t{1});
				expect(after == std::set<int32_t>{0}).to_be_true();
				expect(static_cast<int32_t>(walked.generator->variants().pick[acid])).to_equal(2);
			}
		});
	});
});

CPPSPEC_SPEC(techno_variants)
