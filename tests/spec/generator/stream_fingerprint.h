// A genre's event stream as one number (#226): a hash over many pieces, so work on other genres, subgenres or shared
// tables shows when it changes what a pinned genre plays. Each golden pins its genre's (golden.h);
// acid_fingerprint_spec compares Acid techno with Techno from before the split.

#pragma once

#include "generative/core/golden_case.h"
#include "harness.h"

#include <cstdint>
#include <functional>

namespace harness {

/// A stream's fingerprint, its event count, its drops and evictions, and the pieces that ended by themselves.
struct StreamFingerprint {
	uint64_t hash = 1469598103934665603ULL; ///< FNV-1a's offset basis
	uint64_t events = 0;
	uint32_t lost = 0;
	uint32_t ended = 0;
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

/// Rewrites an event before it's hashed, or returns false to leave it out: so a spec can hash a stream without one
/// part, and compare it with a stream from before the part was added.
using EventFilter = std::function<bool(Event&)>;

/// How fingerprintOf() plays its pieces. The defaults are the goldens' (golden.h); a spec that fingerprints other
/// pieces of a genre changes the salts.
struct FingerprintPlay {
	int32_t bars = 136;
	uint32_t recipeSalt = 13;  ///< Piece n's Recipe comes from entropy n * 7919 + recipeSalt.
	uint32_t drawSalt = 12345; ///< Salts piece n's draws of macros, scale and key.
	bool rollScale = true;     ///< Draw the scale too, or keep the Recipe's.
	/// Each event is seen by it first, if set.
	EventFilter filter;
	/// Sets up piece n's Run after its draws, if set: its steering, a watch.
	std::function<void(uint32_t piece, Run&)> setUp;
};

/// `pieces` pieces of `genre`, `how.bars` bars each, with random macros, scales, keys and seeds, every third from
/// SHIFT+PLAY (skipIntro).
inline StreamFingerprint fingerprintOf(const Genre& genre, uint32_t pieces, const FingerprintPlay& how = {}) {
	StreamFingerprint out;
	for (uint32_t piece = 0; piece < pieces; piece++) {
		Run run;
		run.recipe = newRecipe(genre, piece * 7919U + how.recipeSalt);
		// Each variant set on the golden's variant (spec §3.6): a set added with today's behaviour first moves nothing.
		fixVariants(run.recipe, deluge::generative::golden_case::overrides().variant);
		uint32_t r = piece * 2654435761U + how.drawSalt;
		auto next = [&r] {
			r = r * 1664525U + 1013904223U;
			return r >> 8;
		};
		for (int32_t m = 0; m < kNumMacros; m++) {
			run.recipe.macros.set(static_cast<Macro>(m), static_cast<int32_t>(next() % (macroPoints(50) + 1)));
		}
		if (how.rollScale) {
			run.recipe.scale = static_cast<Scale>(next() % kNumScales);
		}
		run.recipe.keyRoot = static_cast<uint8_t>(next() % 12);
		run.seed = piece + 1;
		run.skipIntro = piece % 3 == 0;
		if (how.setUp) {
			how.setUp(piece, run);
		}
		const Result result = play(run, how.bars);
		out.lost += result.droppedEvents + result.evictedNotes;
		out.ended += result.ended ? 1 : 0;
		for (Event e : result.events) {
			if (how.filter && !how.filter(e)) {
				continue;
			}
			out.events++;
			out.hash = detail::mix(out.hash, e);
		}
	}
	return out;
}

} // namespace harness
