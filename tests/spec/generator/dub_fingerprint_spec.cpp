// Dub techno's event stream, pinned (#221), as acid_fingerprint_spec and melodic_fingerprint_spec pin theirs: so work
// on shared code (the Loop's long notes, the Stab, the pedal Sustain, Extensions) shows when it changes what Dub
// techno plays. A change to its own stream is fine when it's meant: update kDubStream in the same commit and say why
// in its message.

#include "stream_fingerprint.h"

#include "cppspec.hpp"

#include <cstdint>
#include <cstdio>

using namespace harness;

namespace {

/// The fingerprint, its event count and its 0 drops and evictions, measured on #221's head.
constexpr uint64_t kDubStream = 0x6d75ec5c3f5944d3ULL;
constexpr uint64_t kDubEvents = 1882308;

} // namespace

// clang-format off
describe dub_fingerprint("Dub techno's event stream", $ {
	it("is unchanged: 60 pieces with random macros, scales, keys and seeds, every third from SHIFT+PLAY", _ {
		const StreamFingerprint stream = fingerprintOf(genres::technoDub, 60);
		std::printf("dub stream %016llx, %llu events\n", static_cast<unsigned long long>(stream.hash),
		            static_cast<unsigned long long>(stream.events));
		expect(stream.lost).to_equal(0U);
		expect(stream.events).to_equal(kDubEvents);
		expect(stream.hash).to_equal(kDubStream);
	});
});

CPPSPEC_SPEC(dub_fingerprint)
