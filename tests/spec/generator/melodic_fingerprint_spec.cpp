// Melodic techno's event stream, pinned (#129's review), as acid_fingerprint_spec pins Acid techno's: so work on shared
// code (the Loop and Motif lines, the Arp, Extensions, lengths) shows when it changes what Melodic techno plays. A
// change to its own stream is fine when it's meant: update kMelodicStream in the same commit and say why in its
// message.

#include "stream_fingerprint.h"

#include "cppspec.hpp"

#include <cstdint>
#include <cstdio>

using namespace harness;

namespace {

/// The fingerprint, its event count and its 0 drops and evictions. Measured in #232's second round on a tree whose 120
/// pieces still gave 30013d69c9d1f5c0 (3,564,002 events), the value the review measured on 7b0c6815e and its base
/// e07195326. These 60 are the first of those 120, at about half the run time.
constexpr uint64_t kMelodicStream = 0xbc893bad3d7820c4ULL;
constexpr uint64_t kMelodicEvents = 1775191;

} // namespace

// clang-format off
describe melodic_fingerprint("Melodic techno's event stream", $ {
	it("is unchanged: 60 pieces with random macros, scales, keys and seeds, every third from SHIFT+PLAY", _ {
		const StreamFingerprint stream = fingerprintOf(genres::technoMelodic, 60);
		std::printf("melodic stream %016llx, %llu events\n", static_cast<unsigned long long>(stream.hash),
		            static_cast<unsigned long long>(stream.events));
		expect(stream.lost).to_equal(0U);
		expect(stream.events).to_equal(kMelodicEvents);
		expect(stream.hash).to_equal(kMelodicStream);
	});
});

CPPSPEC_SPEC(melodic_fingerprint)
