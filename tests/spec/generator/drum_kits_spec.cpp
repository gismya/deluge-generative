// Drum kits (spec §10.2, §10.3): one drum machine per piece across the Drums lanes, a lane the machine has no file
// for falling back to its own list, and the genre's idiomatic swaps applied some of the time.

#include "fake_card.h"
#include "generative/core/genre.h"
#include "generative/core/palette.h"
#include "generative/core/recipe.h"

#include "cppspec.hpp"

#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace deluge::generative;
using harness::FakeCard;

namespace {

constexpr int32_t at(Lane lane) {
	return static_cast<int32_t>(lane);
}

// ---- A made-up genre: two machines over a small card ----

/// The Kick by its path, so the 909's distorted kick never plays as the machine's (#375).
constexpr KitLane k909Lanes[] = {{Lane::Kick, "SAMPLES/DRUMS/Kick/909 Kick.wav"}};
constexpr DrumMachine k909{.name = "909", .tag = "909", .lanes = k909Lanes};
constexpr KitLane k808Lanes[] = {{Lane::Kick, "SAMPLES/DRUMS/Kick/808 Kick.wav"}};
constexpr DrumMachine k808{.name = "808", .tag = "808", .lanes = k808Lanes};
constexpr MachineWeight kMachines[] = {{&k909, 1}, {&k808, 1}};

constexpr TagWeight kKickTags[] = {{"Deep Sky", 1}};
/// The Snare's list names neither machine: the kit's tag matches its folder's files whatever the list says.
constexpr TagWeight kSnareTags[] = {{"DDD1", 1}};
constexpr TagWeight kRimTags[] = {{"DDD1", 1}};
constexpr TagWeight kClosedHatTags[] = {{"909", 1}, {"808", 1}};
constexpr TagWeight kOpenHatTags[] = {{"909", 1}, {"808", 1}};
constexpr LaneSounds kLanes[] = {
    {.lane = Lane::Kick, .tags = kKickTags},       {.lane = Lane::Snare, .tags = kSnareTags},
    {.lane = Lane::Rim, .tags = kRimTags},         {.lane = Lane::ClosedHat, .tags = kClosedHatTags},
    {.lane = Lane::OpenHat, .tags = kOpenHatTags},
};
constexpr LaneSpec kLaneSpecs[] = {{.lane = Lane::Kick},
                                   {.lane = Lane::Snare},
                                   {.lane = Lane::Rim},
                                   {.lane = Lane::ClosedHat},
                                   {.lane = Lane::OpenHat}};
constexpr PartSpec kParts[] = {{.role = Role::Drums, .name = "DRUMS", .lanes = kLaneSpecs}};

constexpr KitSwap kSwaps[] = {{Lane::Snare, "DDD1"}, {Lane::ClosedHat, "DDD1"}};

constexpr Palette kNoSwaps{.lanes = kLanes, .kits = {.machines = kMachines}};
/// The 909 three times as likely as the 808.
constexpr MachineWeight kWeightedMachines[] = {{&k909, 3}, {&k808, 1}};
constexpr Palette kWeighted{.lanes = kLanes, .kits = {.machines = kWeightedMachines}};
constexpr Palette kHalfSwaps{.lanes = kLanes, .kits = {.machines = kMachines, .swaps = kSwaps}};
constexpr Palette kAlwaysSwaps{.lanes = kLanes, .kits = {.machines = kMachines, .swaps = kSwaps, .swapPercent = 100}};
constexpr Palette kNeverSwaps{.lanes = kLanes, .kits = {.machines = kMachines, .swaps = kSwaps, .swapPercent = 0}};

constexpr Genre genreWith(const Palette& palette) {
	return Genre{.name = "kits", .parts = kParts, .palette = &palette};
}

FakeCard kitCard() {
	FakeCard card;
	card.add("SAMPLES/DRUMS/Kick",
	         {"909 Kick.wav", "909 Kick distorted.wav", "808 Kick.wav", "808 Kick sustained.wav", "Deep Sky.wav"});
	card.add("SAMPLES/DRUMS/Snare", {"909 Snare.wav", "808 Snare.wav", "DDD1 Snare.wav"});
	card.add("SAMPLES/DRUMS/Rim", {"808 Rim.wav", "DDD1 Rim.wav"}); // no 909 rim
	card.add("SAMPLES/DRUMS/HatC", {"909 Closed hihat.wav", "808 Closed hihat.wav", "DDD1 Closed hihat.wav"});
	card.add("SAMPLES/DRUMS/HatO", {"909 Open hihat.wav", "808 Open hihat.wav", "DDD1 Open hihat.wav"});
	return card;
}

SoundPicks rolled(const Genre& genre, uint32_t entropy, FakeCard& card) {
	SoundPicks sounds;
	rollSounds(genre, *genre.palette, sounds, entropy, card);
	return sounds;
}

/// The file name of a card path, without its folder.
std::string fileName(const SoundPath& path) {
	const char* slash = std::strrchr(path.c_str(), '/');
	return slash == nullptr ? path.c_str() : slash + 1;
}

bool startsWith(const std::string& text, const std::string& prefix) {
	return text.rfind(prefix, 0) == 0;
}

// ---- The real genres ----

/// Whether a lane's pick plays `machine`: it matches the machine's tag (or exact path) for that lane.
bool plays(const DrumMachine& machine, Lane lane, const SoundPath& pick) {
	const char* tag = machine.laneTag(lane);
	if (tag == nullptr || pick.empty()) {
		return false;
	}
	return isExactPathTag(tag) ? equalsIgnoringCase(pick.c_str(), tag) : tagMatches(fileName(pick).c_str(), tag);
}

/// Whether `machine` has a file on `card` for `lane`, in the lane's folders.
bool hasFile(const Genre& genre, const DrumMachine& machine, Lane lane, FakeCard& card) {
	const LaneSounds* sounds = genre.palette->laneSounds(lane);
	const char* tag = machine.laneTag(lane);
	if (sounds == nullptr || tag == nullptr) {
		return false;
	}
	TagWeight only[] = {{tag, 1}};
	LaneSounds single = *sounds;
	single.tags = only;
	LaneSounds lanes[] = {single};
	Palette palette{.lanes = lanes};
	LaneSpec specs[] = {{.lane = lane}};
	PartSpec parts[] = {{.role = Role::Drums, .lanes = specs}};
	Genre one{.parts = parts, .palette = &palette};
	SoundPicks picks;
	rollSounds(one, palette, picks, 1, card);
	return !picks.lane[at(lane)].empty();
}

/// The swap the genre lists for `lane`, or nullptr.
const char* swapTag(const Genre& genre, Lane lane) {
	for (const KitSwap& swap : genre.palette->kits.swaps) {
		if (swap.lane == lane) {
			return swap.tag;
		}
	}
	return nullptr;
}

bool isSwap(const Genre& genre, Lane lane, const SoundPath& pick) {
	const char* tag = swapTag(genre, lane);
	if (tag == nullptr) {
		return false;
	}
	return isExactPathTag(tag) ? equalsIgnoringCase(pick.c_str(), tag) : tagMatches(fileName(pick).c_str(), tag);
}

/// The Drums lanes `genre` plays.
std::vector<Lane> drumLanes(const Genre& genre) {
	std::vector<Lane> lanes;
	for (const PartSpec& part : genre.parts) {
		for (const LaneSpec& lane : part.lanes) {
			if (!isFxLane(lane.lane)) {
				lanes.push_back(lane.lane);
			}
		}
	}
	return lanes;
}

/// Whether one of `genre`'s machines plays every Drums lane of `sounds` that it has a file for, a swap aside.
bool oneMachine(const Genre& genre, const SoundPicks& sounds, FakeCard& card) {
	for (const MachineWeight& entry : genre.palette->kits.machines) {
		bool all = true;
		for (Lane lane : drumLanes(genre)) {
			const SoundPath& pick = sounds.lane[at(lane)];
			// The Open hat follows the Closed hat's swap (the hat pair), so it may play the swap's machine.
			const bool swapped = isSwap(genre, lane, pick)
			                     || (lane == Lane::OpenHat && swapTag(genre, Lane::ClosedHat) != nullptr
			                         && isSwap(genre, Lane::ClosedHat, sounds.lane[at(Lane::ClosedHat)]));
			all = all && (swapped || plays(*entry.machine, lane, pick) || !hasFile(genre, *entry.machine, lane, card));
		}
		if (all) {
			return true;
		}
	}
	return false;
}

} // namespace

// clang-format off
describe drum_kits("Drum kits", $ {
	it("plays one machine on every Drums lane of a piece, the Kick by its exact path", _ {
		FakeCard card = kitCard();
		const Genre genre = genreWith(kNoSwaps);
		std::set<std::string> kicks;
		for (uint32_t entropy = 0; entropy < 200; entropy++) {
			SoundPicks sounds = rolled(genre, entropy, card);
			const std::string kick = fileName(sounds.lane[at(Lane::Kick)]);
			kicks.insert(kick);
			const std::string machine = startsWith(kick, "909") ? "909" : "808";
			expect(startsWith(fileName(sounds.lane[at(Lane::Snare)]), machine)).to_be_true();
			expect(startsWith(fileName(sounds.lane[at(Lane::ClosedHat)]), machine)).to_be_true();
			expect(startsWith(fileName(sounds.lane[at(Lane::OpenHat)]), machine)).to_be_true();
		}
		expect(kicks == std::set<std::string>{"909 Kick.wav", "808 Kick.wav"}).to_be_true();
	});

	it("falls back to a lane's own list where the machine has no file", _ {
		FakeCard card = kitCard();
		const Genre genre = genreWith(kNoSwaps);
		std::set<std::string> rimsUnder909;
		for (uint32_t entropy = 0; entropy < 200; entropy++) {
			SoundPicks sounds = rolled(genre, entropy, card);
			if (startsWith(fileName(sounds.lane[at(Lane::Kick)]), "909")) {
				rimsUnder909.insert(fileName(sounds.lane[at(Lane::Rim)]));
			}
		}
		expect(rimsUnder909 == std::set<std::string>{"DDD1 Rim.wav"}).to_be_true();
	});

	it("applies each swap in some pieces and not in others, at the genre's chance", _ {
		FakeCard card = kitCard();
		const Genre half = genreWith(kHalfSwaps);
		int32_t swapped = 0;
		for (uint32_t entropy = 0; entropy < 400; entropy++) {
			swapped += fileName(rolled(half, entropy, card).lane[at(Lane::Snare)]) == "DDD1 Snare.wav" ? 1 : 0;
		}
		expect(swapped).to_be_between(160, 240);
		const Genre always = genreWith(kAlwaysSwaps);
		const Genre never = genreWith(kNeverSwaps);
		for (uint32_t entropy = 0; entropy < 100; entropy++) {
			expect(fileName(rolled(always, entropy, card).lane[at(Lane::Snare)])).to_equal(std::string("DDD1 Snare.wav"));
			expect(fileName(rolled(never, entropy, card).lane[at(Lane::Snare)])).not_().to_equal(std::string("DDD1 Snare.wav"));
		}
	});

	it("rolls each swap on its own, so a piece can take one swap and not another", _ {
		FakeCard card = kitCard();
		const Genre half = genreWith(kHalfSwaps);
		bool snareOnly = false;
		bool hatOnly = false;
		for (uint32_t entropy = 0; entropy < 200; entropy++) {
			SoundPicks sounds = rolled(half, entropy, card);
			const bool snare = startsWith(fileName(sounds.lane[at(Lane::Snare)]), "DDD1");
			const bool hat = startsWith(fileName(sounds.lane[at(Lane::ClosedHat)]), "DDD1");
			snareOnly = snareOnly || (snare && !hat);
			hatOnly = hatOnly || (hat && !snare);
		}
		expect(snareOnly && hatOnly).to_be_true();
	});

	it("pairs the Open hat with a Closed hat swap", _ {
		FakeCard card = kitCard();
		const Genre always = genreWith(kAlwaysSwaps);
		for (uint32_t entropy = 0; entropy < 100; entropy++) {
			SoundPicks sounds = rolled(always, entropy, card);
			expect(fileName(sounds.lane[at(Lane::ClosedHat)])).to_equal(std::string("DDD1 Closed hihat.wav"));
			expect(fileName(sounds.lane[at(Lane::OpenHat)])).to_equal(std::string("DDD1 Open hihat.wav"));
		}
	});

	it("moves a Drums re-roll to another machine", _ {
		FakeCard card = kitCard();
		const Genre genre = genreWith(kNoSwaps);
		for (uint32_t entropy = 0; entropy < 100; entropy++) {
			SoundPicks sounds = rolled(genre, entropy, card);
			const std::string before = fileName(sounds.lane[at(Lane::Snare)]).substr(0, 3);
			rollPartSounds(genre, *genre.palette, 0, sounds, entropy + 1000, card);
			expect(fileName(sounds.lane[at(Lane::Snare)]).substr(0, 3)).not_().to_equal(before);
			expect(fileName(sounds.lane[at(Lane::Kick)]).substr(0, 3)).not_().to_equal(before);
		}
	});

	it("rolls Regenerate's machine by its weights, so it may stay, where NEW SOUND always moves (#395)", _ {
		FakeCard card = kitCard();
		const Genre genre = genreWith(kWeighted);
		int32_t kept = 0;
		int32_t on909 = 0;
		int32_t moved = 0;
		const auto machineOf = [](const SoundPicks& sounds) { return fileName(sounds.lane[at(Lane::Snare)]).substr(0, 3); };
		for (uint32_t entropy = 0; entropy < 400; entropy++) {
			const SoundPicks before = rolled(genre, entropy, card);
			SoundPicks regenerated = before;
			rollIncomingSounds(genre, genre, *genre.palette, regenerated, entropy + 1000, card);
			kept += machineOf(regenerated) == machineOf(before) ? 1 : 0;
			on909 += machineOf(regenerated) == "909" ? 1 : 0;
			SoundPicks renewed = before;
			rollPartSounds(genre, *genre.palette, 0, renewed, entropy + 1000, card);
			moved += machineOf(renewed) != machineOf(before) ? 1 : 0;
		}
		// 909 at 3 in 4 whatever played before: kept 3/4 * 3/4 + 1/4 * 1/4 = 5/8 of the time.
		expect(on909).to_be_between(255, 345);
		expect(kept).to_be_between(200, 300);
		expect(moved).to_equal(400);
	});

	it("gives every genre one main machine per piece, its swaps aside", _ {
		FakeCard card = harness::factoryCardWithPack();
		for (const Genre* genre : allGenres()) {
			if (drumLanes(*genre).empty()) {
				continue;
			}
			expect(genre->palette->kits.machines.empty()).to_be_false();
			for (uint32_t entropy = 0; entropy < 150; entropy++) {
				SoundPicks sounds = rolled(*genre, entropy, card);
				if (!oneMachine(*genre, sounds, card)) {
					std::printf("%s, entropy %u: no one machine\n", genre->name, entropy);
				}
				expect(oneMachine(*genre, sounds, card)).to_be_true();
			}
		}
	});

	it("applies every genre's swaps in some pieces and not in others", _ {
		FakeCard card = harness::factoryCardWithPack();
		for (const Genre* genre : allGenres()) {
			for (const KitSwap& swap : genre->palette->kits.swaps) {
				int32_t on = 0;
				for (uint32_t entropy = 0; entropy < 300; entropy++) {
					on += isSwap(*genre, swap.lane, rolled(*genre, entropy, card).lane[at(swap.lane)]) ? 1 : 0;
				}
				// At least the swap's own chance, and more where the machine or the lane's list plays it too (Psytrance's
				// 909 crash: the Psy set has none), but never every piece.
				if (on < 60 || on > 285) {
					std::printf("%s: swap \"%s\" in %d of 300 pieces\n", genre->name, swap.tag, on);
				}
				expect(on).to_be_between(60, 285);
			}
		}
	});

	it("never plays the machines the drum-kit session took out of a genre", _ {
		FakeCard card = harness::factoryCardWithPack();
		// Techno: no DDD1 main machine. Psytrance: no XV5080 anywhere. Dub: no Linndrum anywhere.
		const std::map<std::string, std::string> removed = {
		    {"techno-acid", "DDD1"}, {"techno-melodic", "DDD1"}, {"techno-dub", "DDD1"}, {"techno-detroit", "DDD1"},
		    {"psytrance-classic", "XV5080"}, {"dub", "Linndrum"}};
		for (const Genre* genre : allGenres()) {
			auto gone = removed.find(genre->name);
			if (gone == removed.end()) {
				continue;
			}
			for (const MachineWeight& entry : genre->palette->kits.machines) {
				expect(std::string(entry.machine->name)).not_().to_equal(gone->second);
			}
			const bool anywhere = gone->second != "DDD1";
			// The lane lists alone too: they are what a lane without the machine plays, and every load's fallback
			// chain, so a bare tag ("Conga") there must not reach the removed machine's files either.
			Palette listsOnly = *genre->palette;
			listsOnly.kits = {};
			for (uint32_t entropy = 0; anywhere && entropy < 300; entropy++) {
				SoundPicks sounds;
				rollSounds(*genre, listsOnly, sounds, entropy, card);
				for (Lane lane : drumLanes(*genre)) {
					const std::string name = fileName(sounds.lane[at(lane)]);
					if (startsWith(name, gone->second)) {
						std::printf("%s: lane list rolls %s\n", genre->name, name.c_str());
					}
					expect(startsWith(name, gone->second)).to_be_false();
				}
			}
			for (uint32_t entropy = 0; entropy < 300; entropy++) {
				SoundPicks sounds = rolled(*genre, entropy, card);
				for (Lane lane : drumLanes(*genre)) {
					const std::string name = fileName(sounds.lane[at(lane)]);
					// DDD1 may stay in Techno's swaps and lane lists; it never plays the Kick or the hats.
					const bool kitLane = lane == Lane::Kick || lane == Lane::ClosedHat || lane == Lane::OpenHat;
					if (anywhere || kitLane) {
						expect(startsWith(name, gone->second)).to_be_false();
					}
				}
			}
		}
	});

	it("plays only the plain 909 kick as Techno's 909 kick (#375)", _ {
		FakeCard card = harness::factoryCardWithPack();
		for (const Genre* genre : allGenres()) {
			if (!startsWith(genre->name, "techno")) {
				continue;
			}
			for (uint32_t entropy = 0; entropy < 300; entropy++) {
				const std::string kick = fileName(rolled(*genre, entropy, card).lane[at(Lane::Kick)]);
				expect(kick == "909 Kick dry.wav" || kick == "909 KICK dry.wav").to_be_false();
				if (startsWith(kick, "909") && kick != "909 Kick.wav") {
					// Only Acid's swap names the distorted one.
					expect(std::string(genre->name)).to_equal(std::string("techno-acid"));
					expect(kick).to_equal(std::string("909 Kick distorted.wav"));
				}
			}
		}
	});
});

CPPSPEC_SPEC(drum_kits)
