// Acid techno (spec §4.5, #218): the default genre, a Techno subgenre that plays today's acid Loop over the parent's
// drums with a Drone under it: a pedal Sustain Pad on the key's root and fifth, which the Break keeps. Run straight
// through the core. That everything but the Drone plays as Techno did before the split is acid_fingerprint_spec's.

#include "core_walk.h"
#include "fake_card.h"
#include "generative/core/palette.h"
#include "harness.h"
#include "stream_helpers.h"

#include "cppspec.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace harness;

namespace {

const Genre& acid = genres::technoAcid;

constexpr int32_t kAcidPart = 1;
constexpr int32_t kDronePart = 2;
constexpr int32_t kFxPart = 3;

Recipe acidRecipeIn(Scale scale, uint8_t keyRoot, uint32_t entropy) {
	Recipe recipe = newRecipe(acid, entropy);
	recipe.keyRoot = keyRoot;
	recipe.scale = scale;
	return recipe;
}

/// Whether a Drone note of pitch class `pitchClass` sounds through the whole bar from `tick`.
bool soundsThrough(const std::vector<Held>& notes, int32_t pitchClass, int32_t tick) {
	return std::any_of(notes.begin(), notes.end(), [&](const Held& note) {
		return note.pitch % 12 == pitchClass && note.on <= tick && note.off >= tick + kBarTicks;
	});
}

} // namespace

// clang-format off
describe acid_techno("Acid techno", $ {
	// Its parent, its place as the default genre and the parent's name are subgenre_spec's identity checks.
	it("is a Techno subgenre of four parts, Drums, Acid, Drone and FX (spec §4.5)", _ {
		expect(std::string(acid.name)).to_equal(std::string("techno-acid"));
		expect(std::string(acid.oledName)).to_equal(std::string("ACID TECHNO"));
		expect(std::string(acid.shortName)).to_equal(std::string("ACID"));
		expect(findGenre("techno-acid") == &acid).to_be_true();
		expect(partCount(acid)).to_equal(4);
		expect(acid.parts[0].role == Role::Drums).to_be_true();
		expect(std::string(acid.parts[kAcidPart].name)).to_equal(std::string("ACID"));
		expect(acid.parts[kAcidPart].role == Role::Bass).to_be_true();
		expect(acid.parts[kAcidPart].flavour == Flavour::Loop).to_be_true();
		expect(std::string(acid.parts[kDronePart].name)).to_equal(std::string("DRONE"));
		expect(acid.parts[kDronePart].role == Role::Pad).to_be_true();
		expect(acid.parts[kFxPart].role == Role::FX).to_be_true();
		expect(acid.facts.tempoLowBpm).to_equal(125);
		expect(acid.facts.tempoHighBpm).to_equal(135);
		std::set<Scale> scales(acid.facts.scales.begin(), acid.facts.scales.end());
		expect(scales == std::set<Scale>{Scale::Minor, Scale::Phrygian}).to_be_true();
		expect(acid.harmony.graph == nullptr).to_be_true(); // one chord throughout
		expect(loopNotesValid(acid)).to_be_true();
	});

	it("takes its drums, fills, Arc and FX from the Techno parent, which holds no pitched part of its own", _ {
		const Genre& parent = *acid.parent;
		expect(parent.version).to_equal(0);
		for (const PartSpec& part : parent.parts) {
			expect(part.role == Role::Drums || part.role == Role::FX).to_be_true();
		}
		expect(acid.parts[0].lanes.data() == parent.parts.front().lanes.data()).to_be_true();
		expect(acid.parts[0].gestures.off).to_equal(parent.parts.front().gestures.off);
		expect(acid.parts[kFxPart].lanes.data() == parent.parts.back().lanes.data()).to_be_true();
		expect(acid.fills.data() == parent.fills.data()).to_be_true();
		expect(acid.fillChance).to_equal(parent.fillChance);
		expect(acid.arc.groovePhrases.low).to_equal(parent.arc.groovePhrases.low);
		expect(acid.arc.buildBars.data() == parent.arc.buildBars.data()).to_be_true();
		expect(acid.arc.roll.lane == parent.arc.roll.lane).to_be_true();
		expect(acid.arc.accentLane == parent.arc.accentLane).to_be_true();
		// The parent's Rest fill drops part 1, the bass every Techno subgenre lists second, between Drums and FX.
		// techno.cpp asserts it for each subgenre at compile time; here the check's edges.
		expect(technoRosterValid(acid)).to_be_true();
		static constexpr PartSpec kDrums{.role = Role::Drums};
		static constexpr PartSpec kBassPart{.role = Role::Bass, .flavour = Flavour::Loop};
		static constexpr PartSpec kPad{.role = Role::Pad, .flavour = Flavour::Sustain};
		static constexpr PartSpec kFx{.role = Role::FX};
		static constexpr PartSpec kValid[] = {kDrums, kBassPart, kPad, kFx};
		static constexpr PartSpec kPadSecond[] = {kDrums, kPad, kBassPart, kFx};
		static constexpr PartSpec kDrumsLast[] = {kBassPart, kPad, kFx, kDrums};
		static constexpr PartSpec kFxFirst[] = {kFx, kBassPart, kPad, kDrums};
		static constexpr PartSpec kTwo[] = {kDrums, kFx};
		expect(technoRosterValid(Genre{.parts = kValid})).to_be_true();
		expect(technoRosterValid(Genre{.parts = kPadSecond})).to_be_false();
		expect(technoRosterValid(Genre{.parts = kDrumsLast})).to_be_false();
		expect(technoRosterValid(Genre{.parts = kFxFirst})).to_be_false();
		expect(technoRosterValid(Genre{.parts = kTwo})).to_be_false();
	});

	context("its Drone (spec §4.5, §5.2)", _ {
		it("is a pedal Sustain Pad, not gated, never re-struck on a period, with an 8-bar Brightness Sweep", _ {
			const PartSpec& drone = acid.parts[kDronePart];
			expect(drone.flavour == Flavour::Sustain).to_be_true();
			expect(drone.sustain.pedal).to_be_true();
			expect(drone.sustain.retriggerBars).to_equal(0);
			expect(drone.sustain.doubleRoot).to_be_false();
			expect(drone.densityGate).to_equal(0);
			expect(drone.drift.sweepBars).to_equal(8);
			expect((acid.arc.breakKeeps.parts & partBit(kDronePart)) != 0).to_be_true();
		});

		it("holds the key's root and fifth through every Groove bar, never anything else, and never 3 at once", _ {
			int32_t grooveBars = 0;
			for (Scale scale : acid.facts.scales) {
				for (uint8_t keyRoot : {0, 4, 9}) {
					for (uint32_t seed = 1; seed <= 3; seed++) {
						const Recipe recipe = acidRecipeIn(scale, keyRoot, seed * 31U + keyRoot);
						const Walked walked = walk(recipe, seed, 96 * kStepsPerBar);
						expect(walked.dropped).to_equal(0U);
						expect(walked.evicted).to_equal(0U);
						const Register reg = acid.parts[kDronePart].registerOrDefault();
						const int32_t tonic = tonicNote(reg, keyRoot);
						const std::vector<Held> notes = heldNotes(walked.notes, kDronePart);
						std::map<int32_t, int32_t> sounding; // tick → notes sounding from it
						for (const Held& note : notes) {
							// The root where the Register puts it, and the fifth above it: nothing moves them.
							expect(note.pitch == tonic || note.pitch == tonic + 7).to_be_true();
							expect(note.on % kBarTicks).to_equal(0);
						}
						for (size_t n = 0; n + kStepsPerBar < walked.steps.size(); n += kStepsPerBar) {
							const int32_t tick = stepTick(static_cast<int32_t>(n));
							int32_t at = 0;
							for (const Held& note : notes) {
								at += note.on <= tick && note.off > tick ? 1 : 0;
							}
							expect(at).to_be_less_than(3);
							if (walked.steps[n].section != SectionKind::Groove) {
								continue;
							}
							grooveBars++;
							expect(soundsThrough(notes, keyRoot, tick)).to_be_true();
							expect(soundsThrough(notes, (keyRoot + 7) % 12, tick)).to_be_true();
						}
					}
				}
			}
			expect(grooveBars).to_be_greater_than(500);
		});

		it("holds each note across bar lines, re-striking only on a Section line", _ {
			int32_t held = 0;
			int32_t restruck = 0;
			for (uint32_t seed = 1; seed <= 4; seed++) {
				const Walked walked = walk(acidRecipeIn(Scale::Minor, 9, seed), seed, 96 * kStepsPerBar);
				const std::vector<Held> notes = heldNotes(walked.notes, kDronePart);
				for (const Held& note : notes) {
					held += note.off - note.on > 4 * kBarTicks ? 1 : 0;
					const bool again = std::any_of(notes.begin(), notes.end(), [&](const Held& before) {
						return before.pitch == note.pitch && before.off == note.on;
					});
					if (!again) {
						continue; // a note Density brings in, or the first
					}
					// The same pitch, ended and struck again on one line: only a Section line does that.
					restruck++;
					const int32_t step = note.on / kTicksPerStep;
					expect(walked.steps[std::min<size_t>(step, walked.steps.size() - 1)].sectionStart).to_be_true();
				}
			}
			expect(held).to_be_greater_than(10);
			expect(restruck).to_be_greater_than(10);
		});
	});

	it("plays its Drone alone with the kick, rim and closed hat in a Break, and the Drone sounds through it", _ {
		int32_t breakBars = 0;
		int32_t droneNotesInBreaks = 0;
		for (uint32_t seed = 1; seed <= 8; seed++) {
			const Recipe recipe = acidRecipeIn(Scale::Minor, 9, seed);
			const Walked walked = walk(recipe, seed, 120 * kStepsPerBar);
			const std::vector<Held> notes = heldNotes(walked.notes, kDronePart);
			for (const Event& e : walked.noteOns) {
				const int32_t step = std::min<int32_t>(stepOf(e), static_cast<int32_t>(walked.steps.size()) - 1);
				if (walked.steps[step].section != SectionKind::Break || walked.steps[step].sectionStart) {
					continue;
				}
				droneNotesInBreaks += e.part == kDronePart ? 1 : 0;
				const bool kept = e.part == kDronePart || e.part == kFxPart
				                  || (e.part == 0
				                      && (e.lane == Lane::Kick || e.lane == Lane::Rim || e.lane == Lane::ClosedHat));
				expect(kept).to_be_true();
			}
			for (size_t n = 0; n + kStepsPerBar < walked.steps.size(); n += kStepsPerBar) {
				if (walked.steps[n].section != SectionKind::Break) {
					continue;
				}
				breakBars++;
				expect(soundsThrough(notes, recipe.keyRoot, stepTick(static_cast<int32_t>(n)))).to_be_true();
			}
		}
		expect(breakBars).to_be_greater_than(30);
	});

	it("keeps the Drone's fifth in a Break from Density 35, a bare root below: the parent's Break dip thins it", _ {
		// The Break dips Density by 20 (the parent's, so the kept drums play as Techno's did): at the default 25 only
		// anchors play, the Drone's root among them (spec §8.1).
		for (int32_t density : {25, 35}) {
			int32_t breakBars = 0;
			int32_t withFifth = 0;
			for (uint32_t seed = 1; seed <= 6; seed++) {
				Recipe recipe = acidRecipeIn(Scale::Minor, 9, seed);
				recipe.macros.set(Macro::Density, macroPoints(density));
				const Walked walked = walk(recipe, seed, 120 * kStepsPerBar);
				const std::vector<Held> notes = heldNotes(walked.notes, kDronePart);
				for (size_t n = 0; n + kStepsPerBar < walked.steps.size(); n += kStepsPerBar) {
					if (walked.steps[n].section == SectionKind::Break) {
						breakBars++;
						withFifth += soundsThrough(notes, 4, stepTick(static_cast<int32_t>(n))) ? 1 : 0; // E over A
					}
				}
			}
			expect(breakBars).to_be_greater_than(30);
			expect(withFifth).to_equal(density == 25 ? 0 : breakBars);
		}
	});

	context("its palette (spec §10.1, §18.3)", _ {
		it("picks the acid presets for ACID and the drone pads for the DRONE, all on the factory card", _ {
			FakeCard card = harness::factoryCardWithPack();
			const auto acidEntries = acid.palette->partEntries(acid.parts[kAcidPart]);
			expect(std::string(acidEntries[0].path)).to_equal(std::string("SYNTHS/165 Acid Arp.XML"));
			const auto droneEntries = acid.palette->partEntries(acid.parts[kDronePart]);
			expect(droneEntries.size()).to_equal(size_t{4});
			std::set<std::string> drones;
			for (const PaletteEntry& entry : droneEntries) {
				expect(card.has(entry.path)).to_be_true();
				drones.insert(entry.path);
			}
			expect(drones.count("SYNTHS/148 Warm 5th Pad.XML")).to_equal(size_t{1});
		});

		it("takes its drum and FX lanes from the parent's palette, which lists no synth part", _ {
			const Palette& parent = *acid.parent->palette;
			expect(parent.parts.empty()).to_be_true();
			expect(acid.palette->lanes.data() == parent.lanes.data()).to_be_true();
			expect(genres::technoMelodic.palette->lanes.data() == parent.lanes.data()).to_be_true();
		});
	});
});

CPPSPEC_SPEC(acid_techno)
