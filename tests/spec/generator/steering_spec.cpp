// The steering queue and log (spec §2.6, ADR 0005).

#include "generative/core/recipe.h"
#include "generative/core/steering.h"

#include "cppspec.hpp"

using namespace deluge::generative;

namespace {

Command macro(Macro which, int32_t value) {
	return {CommandKind::SetMacro, static_cast<uint8_t>(which), value};
}

} // namespace

// clang-format off
describe steering("The steering queue", $ {
	it("hands commands to the core oldest first", _ {
		SteeringQueue queue;
		queue.push(macro(Macro::Energy, 1));
		queue.push(macro(Macro::Density, 2));
		Command out;
		expect(queue.pop(&out)).to_be_true();
		expect(out == macro(Macro::Energy, 1)).to_be_true();
		expect(queue.pop(&out)).to_be_true();
		expect(out == macro(Macro::Density, 2)).to_be_true();
		expect(queue.pop(&out)).to_be_false();
	});

	it("coalesces a continuous value by key, keeping the latest value in the first one's place", _ {
		SteeringQueue queue;
		queue.push(macro(Macro::Energy, 1));
		queue.push(macro(Macro::Density, 2));
		queue.push(macro(Macro::Energy, 3));
		expect(queue.size()).to_equal(2);
		Command out;
		queue.pop(&out);
		expect(out == macro(Macro::Energy, 3)).to_be_true();
	});

	it("holds 64 commands, and still coalesces into a full queue", _ {
		SteeringQueue queue;
		for (int32_t i = 0; i < 64; i++) {
			// 64 distinct keys: four macros can't fill it, so use made-up targets.
			expect(queue.push({CommandKind::SetMacro, static_cast<uint8_t>(i), i})).to_be_true();
		}
		expect(queue.push({CommandKind::SetMacro, 200, 0})).to_be_false();
		expect(queue.push({CommandKind::SetMacro, 5, 99})).to_be_true();
		expect(queue.size()).to_equal(64);
	});

	it("logs each command with its step until the log is full", _ {
		LogEntry entries[2];
		SteeringLog log(entries, 2);
		expect(log.append(10, macro(Macro::Motion, 7))).to_be_true();
		expect(log.append(11, macro(Macro::Motion, 8))).to_be_true();
		expect(log.full()).to_be_false();
		expect(log.append(12, macro(Macro::Motion, 9))).to_be_false();
		expect(log.full()).to_be_true();
		expect(log.size()).to_equal(2);
		expect(log.at(1).step).to_equal(11);
	});

	it("keeps one pending slot for the key and the force, one per part for takes, and refuses past its capacity", _ {
		PendingList pending;
		expect(pending.add({CommandKind::SetKey, 0, 1})).to_be_true();
		expect(pending.add({CommandKind::SetKey, 2, 1})).to_be_true();
		expect(pending.add({CommandKind::ForceSection, 2, 0})).to_be_true();
		expect(pending.add({CommandKind::ForceSection, 3, 128})).to_be_true();
		expect(pending.size()).to_equal(2);
		expect(pending.waiting({CommandKind::SetKey, 9, 0})->target).to_equal(uint8_t{2});
		for (uint8_t part = 0; part < PendingList::kCapacity - 2; part++) {
			expect(pending.add({CommandKind::SetTake, part, 1})).to_be_true();
		}
		expect(pending.size()).to_equal(PendingList::kCapacity);
		expect(pending.add({CommandKind::SetTake, 0, 5})).to_be_true(); // its slot is there: replaced
		expect(pending.add({CommandKind::SetTake, PendingList::kCapacity, 1})).to_be_false();
		expect(pending.size()).to_equal(PendingList::kCapacity);
	});
});

CPPSPEC_SPEC(steering)
