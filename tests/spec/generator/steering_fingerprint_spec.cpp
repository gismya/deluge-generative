// What each Techno subgenre plays under steering (spec §12.5, §13.4, §8.7, §9.6, §13.2): a stream fingerprint of
// pieces with a mute, a solo, a SHIFT + mute, a held fill (Techno's held entry, a Rest, and a fresh pick, which holds
// a Swap in one piece, checked), a force and End, each pressed and released. The goldens play without steering, so this
// pins how the silencers combine with a mute, a solo, a held fill, a force and End. A retune that moves a stream moves
// its fingerprint here too: update it with the genre's golden (spec §21.2).

#include "harness.h"
#include "stream_fingerprint.h"

#include "cppspec.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

using namespace harness;

namespace {

/// The pieces played per subgenre, and their length: past the Outro that End starts.
constexpr uint32_t kPieces = 6;
constexpr int32_t kBars = 112;

/// The presses for piece `piece`, keyed on the step about to be decided: each pressed and later released, staggered
/// per piece so they land on different lines.
std::vector<std::pair<int32_t, Command>> pressesFor(uint32_t piece) {
	const auto at = [piece](int32_t step) { return step + static_cast<int32_t>(piece) * 13; };
	const auto section = static_cast<uint8_t>(piece % 2 == 0 ? SectionKind::Break : SectionKind::Build);
	return {
	    {at(150), {CommandKind::SetMute, 1, kPress}},         {at(300), {CommandKind::SetSolo, 0, kPress}},
	    {at(420), {CommandKind::SetMute, 1, kPress}},         {at(560), {CommandKind::SetSolo, 0, kPress}},
	    {at(600), {CommandKind::SetMute, 2, kPressNow}},      {at(650), {CommandKind::SetMute, 2, kPressNow}},
	    {at(700), {CommandKind::SetFill, 0, kFillHeldEntry}}, {at(760), {CommandKind::SetFill, 0, kFillReleased}},
	    {at(800), {CommandKind::SetFill, 0, kFillFreshPick}}, {at(830), {CommandKind::SetFill, 0, kFillReleased}},
	    {at(900), {CommandKind::ForceSection, section, 0}},   {at(1200), {CommandKind::End, 0, 1}},
	};
}

/// What FILL held in a steered run: the pieces where it held a Rest, and a Swap.
struct Held {
	uint32_t rests = 0;
	uint32_t swaps = 0;
};

/// `kPieces` steered pieces of `genre`, with random macros, keys and seeds, every third from SHIFT+PLAY. `held` counts
/// what FILL held.
StreamFingerprint steeredFingerprintOf(const Genre& genre, Held& held) {
	std::vector<bool> rested(kPieces);
	std::vector<bool> swapped(kPieces);
	FingerprintPlay how{.bars = kBars, .recipeSalt = 17, .drawSalt = 54321, .rollScale = false};
	how.setUp = [&](uint32_t piece, Run& run) {
		run.steer = steerAt(pressesFor(piece));
		run.watch = [&genre, &rested, &swapped, piece](const Generator& generator) {
			const int32_t fill = generator.heldFill();
			if (fill >= 0) {
				(genre.fills[fill].kind == FillKind::Swap ? swapped : rested)[piece] = true;
			}
		};
	};
	const StreamFingerprint out = fingerprintOf(genre, kPieces, how);
	held.rests = static_cast<uint32_t>(std::ranges::count(rested, true));
	held.swaps = static_cast<uint32_t>(std::ranges::count(swapped, true));
	return out;
}

struct Pinned {
	const Genre* genre;
	uint64_t hash;
	uint64_t events;
};

/// Each subgenre's steered fingerprint; a retune copies the printed value here (spec §21.2).
const Pinned kPinned[] = {
    {&genres::technoAcid, 0xb7a048455dac92e3ULL, 75865},
    {&genres::technoDub, 0xb38078184182eb94ULL, 95566},
    {&genres::technoMelodic, 0x276f79e4e950f4e1ULL, 87623},
    {&genres::technoDetroit, 0xa661f0db3c746c7fULL, 103177},
};

std::string hex(uint64_t value) {
	char text[19];
	std::snprintf(text, sizeof text, "0x%016llx", static_cast<unsigned long long>(value));
	return text;
}

} // namespace

// clang-format off
describe steering_fingerprint("A steered stream's fingerprint", $ {
	it("is pinned for every Techno subgenre, under a mute, a solo, SHIFT + mute, a held fill, a force and End", _ {
		for (const Pinned& pinned : kPinned) {
			Held held;
			const StreamFingerprint fingerprint = steeredFingerprintOf(*pinned.genre, held);
			const std::string name = pinned.genre->name;
			expect(name + " " + hex(fingerprint.hash) + " " + std::to_string(fingerprint.events))
			    .to_equal(name + " " + hex(pinned.hash) + " " + std::to_string(pinned.events));
			// FILL holds Techno's held entry, a Rest, in every piece, and its fresh pick a Swap in one.
			expect(name + " rests " + std::to_string(held.rests) + " swaps " + std::to_string(held.swaps))
			    .to_equal(name + " rests " + std::to_string(kPieces) + " swaps 1");
			expect(fingerprint.lost).to_equal(0U);
			expect(fingerprint.ended).to_equal(kPieces);
		}
	});
});

CPPSPEC_SPEC(steering_fingerprint)
