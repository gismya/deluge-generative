// Techno's event stream, pinned (#226): a fingerprint over many pieces, so work on other genres, subgenres or shared
// tables shows when it changes what Techno plays. A change to Techno's own stream is fine when it's meant: update
// kTechnoStream in the same commit and say why in its message.

#include "harness.h"

#include "cppspec.hpp"

#include <cstdint>
#include <cstdio>

using namespace harness;

namespace {

/// The fingerprint, its event count and its 0 drops and evictions, measured on 586687df0 (the base of #226).
constexpr uint64_t kTechnoStream = 0x6c96b6b2967c7984ULL;
constexpr uint64_t kTechnoEvents = 1342372;

/// FNV-1a over 64 bits, one field at a time.
uint64_t mix(uint64_t hash, uint64_t value) {
	return (hash ^ value) * 1099511628211ULL;
}

uint64_t mix(uint64_t hash, const Event& e) {
	hash = mix(hash, static_cast<uint32_t>(e.tick));
	hash = mix(hash, static_cast<uint64_t>(e.kind));
	hash = mix(hash, e.part);
	hash = mix(hash, static_cast<uint64_t>(e.lane));
	hash = mix(hash, e.pitch);
	hash = mix(hash, e.velocity);
	hash = mix(hash, (e.muted ? 1U : 0U) | (e.ornament ? 2U : 0U) | (e.snap ? 4U : 0U));
	hash = mix(hash, static_cast<uint64_t>(e.control));
	hash = mix(hash, static_cast<uint64_t>(e.section));
	hash = mix(hash, e.occurrence);
	hash = mix(hash, static_cast<uint8_t>(e.offset));
	return mix(hash, static_cast<uint64_t>(e.scale));
}

} // namespace

// clang-format off
describe techno_fingerprint("Techno's event stream", $ {
	it("is unchanged: 60 pieces with random macros, scales, keys and seeds, every third from SHIFT+PLAY", _ {
		uint64_t hash = 1469598103934665603ULL;
		uint64_t events = 0;
		uint32_t lost = 0;
		for (uint32_t piece = 0; piece < 60; piece++) {
			Run run;
			run.recipe = newRecipe(genres::techno, piece * 7919U + 13U);
			uint32_t r = piece * 2654435761U + 12345U;
			auto next = [&r] {
				r = r * 1664525U + 1013904223U;
				return r >> 8;
			};
			for (int32_t m = 0; m < kNumMacros; m++) {
				run.recipe.macros.set(static_cast<Macro>(m), static_cast<int32_t>(next() % (macroPoints(50) + 1)));
			}
			run.recipe.scale = static_cast<Scale>(next() % kNumScales);
			run.recipe.keyRoot = static_cast<uint8_t>(next() % 12);
			run.seed = piece + 1;
			run.skipIntro = piece % 3 == 0;
			const Result result = play(run, 136);
			lost += result.droppedEvents + result.evictedNotes;
			for (const Event& e : result.events) {
				events++;
				hash = mix(hash, e);
			}
		}
		std::printf("techno stream %016llx, %llu events\n", static_cast<unsigned long long>(hash),
		            static_cast<unsigned long long>(events));
		expect(lost).to_equal(0U);
		expect(events).to_equal(kTechnoEvents);
		expect(hash).to_equal(kTechnoStream);
	});
});

CPPSPEC_SPEC(techno_fingerprint)
