// Techno's event stream, pinned (#226): a fingerprint over many pieces, so work on other genres, subgenres or shared
// tables shows when it changes what Techno plays. A change to Techno's own stream is fine when it's meant: update
// kTechnoStream in the same commit and say why in its message.

#include "stream_fingerprint.h"

#include "cppspec.hpp"

#include <cstdint>
#include <cstdio>

using namespace harness;

namespace {

/// The fingerprint, its event count and its 0 drops and evictions, measured on 586687df0 (the base of #226).
constexpr uint64_t kTechnoStream = 0x6c96b6b2967c7984ULL;
constexpr uint64_t kTechnoEvents = 1342372;

} // namespace

// clang-format off
describe techno_fingerprint("Techno's event stream", $ {
	it("is unchanged: 60 pieces with random macros, scales, keys and seeds, every third from SHIFT+PLAY", _ {
		const StreamFingerprint stream = fingerprintOf(genres::techno, 60);
		std::printf("techno stream %016llx, %llu events\n", static_cast<unsigned long long>(stream.hash),
		            static_cast<unsigned long long>(stream.events));
		expect(stream.lost).to_equal(0U);
		expect(stream.events).to_equal(kTechnoEvents);
		expect(stream.hash).to_equal(kTechnoStream);
	});
});

CPPSPEC_SPEC(techno_fingerprint)
