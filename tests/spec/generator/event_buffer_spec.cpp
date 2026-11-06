// The event buffer (spec §2.7, §12.6): play order, and the overflow drop order every driver shares.

#include "generative/core/event_buffer.h"

#include "cppspec.hpp"

using namespace deluge::generative;

namespace {

Event hit(int32_t tick, uint8_t part, bool ornament = false) {
	Event e = Event::noteOn(tick, part, Lane::None, 60, 100);
	e.ornament = ornament;
	return e;
}

} // namespace

// clang-format off
describe event_buffer("The event buffer", $ {
	it("plays a tick's Section first, then note-offs, then note-ons in the order they were decided", _ {
		EventBuffer<8> buffer;
		Event dropped;
		buffer.push(hit(24, 0), Role::Drums, &dropped);
		buffer.push(hit(24, 1), Role::Bass, &dropped);
		buffer.push(Event::noteOff(24, 1, Lane::None, 60), Role::Bass, &dropped);
		buffer.push(Event::sectionStart(24, SectionKind::Groove, 0), Role::Drums, &dropped);
		buffer.push(hit(12, 1), Role::Bass, &dropped);

		expect(buffer.nextTick()).to_equal(12);
		Event out;
		expect(buffer.popDue(11, &out)).to_be_false();
		expect(buffer.popDue(30, &out)).to_be_true();
		expect(out.tick).to_equal(12);
		expect(buffer.popDue(30, &out)).to_be_true();
		expect(out.kind).to_equal(EventKind::Section);
		expect(buffer.popDue(30, &out)).to_be_true();
		expect(out.kind).to_equal(EventKind::NoteOff);
		expect(buffer.popDue(30, &out)).to_be_true();
		expect(out.part).to_equal(uint8_t{0});
		expect(buffer.popDue(30, &out)).to_be_true();
		expect(out.part).to_equal(uint8_t{1});
		expect(buffer.popDue(30, &out)).to_be_false();
	});

	it("drops a full step's Ornament hits first, then parts in reverse role priority", _ {
		EventBuffer<4> buffer;
		Event dropped;
		buffer.beginStep();
		expect(buffer.push(hit(0, 0), Role::Drums, &dropped)).to_be_false();
		expect(buffer.push(hit(0, 1, true), Role::Drums, &dropped)).to_be_false();
		expect(buffer.push(hit(0, 2), Role::FX, &dropped)).to_be_false();
		expect(buffer.push(hit(0, 3), Role::Pad, &dropped)).to_be_false();

		// A Bass note outranks the Drums ornament, which goes first.
		expect(buffer.push(hit(0, 4), Role::Bass, &dropped)).to_be_true();
		expect(dropped.part).to_equal(uint8_t{1});
		// Then FX, then Pad.
		expect(buffer.push(hit(0, 5), Role::Lead, &dropped)).to_be_true();
		expect(dropped.part).to_equal(uint8_t{2});
		expect(buffer.push(hit(0, 6), Role::Chords, &dropped)).to_be_true();
		expect(dropped.part).to_equal(uint8_t{3});
		// Nothing left ranks below another Pad note, so it is the one refused.
		Event pad = hit(0, 7);
		expect(buffer.push(pad, Role::Pad, &dropped)).to_be_true();
		expect(dropped == pad).to_be_true();
	});

	it("never drops events of the step still playing", _ {
		EventBuffer<2> buffer;
		Event dropped;
		buffer.beginStep();
		buffer.push(hit(0, 0), Role::FX, &dropped);
		buffer.push(hit(0, 1), Role::FX, &dropped);
		buffer.beginStep();
		Event drums = hit(24, 2);
		expect(buffer.push(drums, Role::Drums, &dropped)).to_be_true();
		expect(dropped == drums).to_be_true();
	});
});

CPPSPEC_SPEC(event_buffer)
