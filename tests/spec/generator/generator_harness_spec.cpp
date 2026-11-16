#include <type_traits>

#include "cppspec.hpp"

// The harness builds the Generator like the firmware does (spec §21.2, ADR 0004).

// clang-format off
describe generator_harness("Generator harness", $ {
	it("builds with signed char, like the firmware", _ {
		expect(std::is_signed_v<char>).to_be_true();
	});
});

CPPSPEC_SPEC(generator_harness)
