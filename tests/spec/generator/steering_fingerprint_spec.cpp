// What each Techno subgenre plays under steering (spec §12.5, §13.4, §8.7, §9.6, §13.2): a stream fingerprint of
// pieces with a mute, a solo, a SHIFT + mute, a held fill (Techno's held entry, a Rest, and a fresh pick, a Rest or a
// Swap), a force and End, each pressed and released. The goldens play without steering, so this pins how the
// silencers combine with a mute, a solo, a held fill, a force and End. A retune that moves a stream moves its
// fingerprint here too: update it with the genre's golden (spec §21.2).

#include "harness.h"
#include "stream_fingerprint.h"

#include "cppspec.hpp"

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

/// `kPieces` steered pieces of `genre`, with random macros, keys and seeds, every third from SHIFT+PLAY. `ended`
/// counts the pieces that End stopped.
StreamFingerprint steeredFingerprintOf(const Genre& genre, uint32_t& ended) {
	StreamFingerprint out;
	for (uint32_t piece = 0; piece < kPieces; piece++) {
		Run run;
		run.recipe = newRecipe(genre, piece * 7919U + 17U);
		uint32_t r = piece * 2654435761U + 54321U;
		auto next = [&r] {
			r = r * 1664525U + 1013904223U;
			return r >> 8;
		};
		for (int32_t m = 0; m < kNumMacros; m++) {
			run.recipe.macros.set(static_cast<Macro>(m), static_cast<int32_t>(next() % (macroPoints(50) + 1)));
		}
		run.recipe.keyRoot = static_cast<uint8_t>(next() % 12);
		run.seed = piece + 1;
		run.skipIntro = piece % 3 == 0;
		run.steer = steerAt(pressesFor(piece));
		const Result result = play(run, kBars);
		ended += result.ended ? 1 : 0;
		out.lost += result.droppedEvents + result.evictedNotes;
		for (const Event& e : result.events) {
			out.events++;
			out.hash = harness::detail::mix(out.hash, e);
		}
	}
	return out;
}

struct Pinned {
	const Genre* genre;
	uint64_t hash;
	uint64_t events;
};

/// Measured on the code before #259's refactor, which must leave every stream where it was.
const Pinned kPinned[] = {
    {&genres::technoAcid, 0xb7a048455dac92e3ULL, 75865},
    {&genres::technoDub, 0x9734badccb7a5af4ULL, 94742},
    {&genres::technoMelodic, 0x472ded0b2606b601ULL, 87623},
    {&genres::technoDetroit, 0x7adb9e02d3b0865fULL, 103177},
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
			uint32_t ended = 0;
			const StreamFingerprint fingerprint = steeredFingerprintOf(*pinned.genre, ended);
			const std::string name = pinned.genre->name;
			expect(name + " " + hex(fingerprint.hash) + " " + std::to_string(fingerprint.events))
			    .to_equal(name + " " + hex(pinned.hash) + " " + std::to_string(pinned.events));
			expect(fingerprint.lost).to_equal(0U);
			expect(ended).to_equal(kPieces);
		}
	});
});

CPPSPEC_SPEC(steering_fingerprint)
