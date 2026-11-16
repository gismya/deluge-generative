// Detroit techno's event stream, pinned (#220), as acid_fingerprint_spec, melodic_fingerprint_spec and
// dub_fingerprint_spec pin theirs: so work on shared code (the chord walk, the Stab, the Sustain, the Motif,
// Extensions) shows when it changes what Detroit techno plays. A change to its own stream is fine when it's meant:
// update kDetroitStream in the same commit and say why in its message.

#include "stream_fingerprint.h"

#include "cppspec.hpp"

#include <cstdint>
#include <cstdio>

using namespace harness;

namespace {

/// The fingerprint, its event count and its 0 drops and evictions, measured on #220's head.
constexpr uint64_t kDetroitStream = 0xf9bd84c9c88c83d3ULL;
constexpr uint64_t kDetroitEvents = 2002139;

} // namespace

// clang-format off
describe detroit_fingerprint("Detroit techno's event stream", $ {
	it("is unchanged: 60 pieces with random macros, scales, keys and seeds, every third from SHIFT+PLAY", _ {
		const StreamFingerprint stream = fingerprintOf(genres::technoDetroit, 60);
		std::printf("detroit stream %016llx, %llu events\n", static_cast<unsigned long long>(stream.hash),
		            static_cast<unsigned long long>(stream.events));
		expect(stream.lost).to_equal(0U);
		expect(stream.events).to_equal(kDetroitEvents);
		expect(stream.hash).to_equal(kDetroitStream);
	});
});

CPPSPEC_SPEC(detroit_fingerprint)
