// A genre's event stream as one number (#226): a hash over many pieces, so work on other genres, subgenres or shared
// tables shows when it changes what a pinned genre plays. techno_fingerprint_spec and melodic_fingerprint_spec use it.

#pragma once

#include "harness.h"

#include <cstdint>

namespace harness {

/// A stream's fingerprint, its event count and its drops and evictions.
struct StreamFingerprint {
	uint64_t hash = 1469598103934665603ULL; ///< FNV-1a's offset basis
	uint64_t events = 0;
	uint32_t lost = 0;
};

namespace detail {

/// FNV-1a over 64 bits, one field at a time.
inline uint64_t mix(uint64_t hash, uint64_t value) {
	return (hash ^ value) * 1099511628211ULL;
}

inline uint64_t mix(uint64_t hash, const Event& e) {
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

} // namespace detail

/// `pieces` pieces of `genre`, 136 bars each, with random macros, scales, keys and seeds, every third from
/// SHIFT+PLAY (skipIntro).
inline StreamFingerprint fingerprintOf(const Genre& genre, uint32_t pieces) {
	StreamFingerprint out;
	for (uint32_t piece = 0; piece < pieces; piece++) {
		Run run;
		run.recipe = newRecipe(genre, piece * 7919U + 13U);
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
		out.lost += result.droppedEvents + result.evictedNotes;
		for (const Event& e : result.events) {
			out.events++;
			out.hash = detail::mix(out.hash, e);
		}
	}
	return out;
}

} // namespace harness
