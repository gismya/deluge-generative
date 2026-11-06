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
		buffer.pushNoteOn(hit(24, 0), Role::Drums);
		buffer.pushNoteOn(hit(24, 1), Role::Bass);
		buffer.push(Event::noteOff(24, 1, Lane::None, 60));
		buffer.push(Event::sectionStart(24, SectionKind::Groove, 0));
		buffer.pushNoteOn(hit(12, 1), Role::Bass);

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
		buffer.beginStep();
		expect(buffer.pushNoteOn(hit(0, 0), Role::Drums).dropped).to_be_false();
		expect(buffer.pushNoteOn(hit(0, 1, true), Role::Drums).dropped).to_be_false();
		expect(buffer.pushNoteOn(hit(0, 2), Role::FX).dropped).to_be_false();
		expect(buffer.pushNoteOn(hit(0, 3), Role::Pad).dropped).to_be_false();

		// A Bass note outranks the Drums ornament, which goes first.
		PushResult result = buffer.pushNoteOn(hit(0, 4), Role::Bass);
		expect(result.stored && result.dropped).to_be_true();
		expect(result.droppedEvent.part).to_equal(uint8_t{1});
		// Then FX, then Pad.
		expect(buffer.pushNoteOn(hit(0, 5), Role::Lead).droppedEvent.part).to_equal(uint8_t{2});
		expect(buffer.pushNoteOn(hit(0, 6), Role::Chords).droppedEvent.part).to_equal(uint8_t{3});
		// Nothing left ranks below another Pad note, so it is the one refused.
		Event pad = hit(0, 7);
		result = buffer.pushNoteOn(pad, Role::Pad);
		expect(result.stored).to_be_false();
		expect(result.droppedEvent == pad).to_be_true();
	});

	it("never drops a note-on of the step still playing to make room for another note-on", _ {
		EventBuffer<2> buffer;
		buffer.beginStep();
		buffer.pushNoteOn(hit(0, 0), Role::FX);
		buffer.pushNoteOn(hit(0, 1), Role::FX);
		buffer.beginStep();
		PushResult result = buffer.pushNoteOn(hit(24, 2), Role::Drums);
		expect(result.stored).to_be_false();
		expect(result.droppedEvent.part).to_equal(uint8_t{2});
	});

	it("never refuses a note-off, Section or key event: the lowest-ranked note-on anywhere makes room", _ {
		EventBuffer<3> buffer;
		buffer.beginStep();
		buffer.pushNoteOn(hit(0, 0), Role::Drums);
		buffer.pushNoteOn(hit(0, 1), Role::FX);
		buffer.beginStep();
		buffer.pushNoteOn(hit(24, 2), Role::Bass);
		PushResult result = buffer.push(Event::noteOff(20, 5, Lane::None, 60));
		expect(result.stored && result.dropped).to_be_true();
		expect(result.droppedEvent.part).to_equal(uint8_t{1}); // the FX note, though it belongs to the earlier step
		result = buffer.push(Event::sectionStart(24, SectionKind::Break, 0));
		expect(result.stored).to_be_true();
		expect(result.droppedEvent.part).to_equal(uint8_t{2});
		expect(buffer.size()).to_equal(3);
	});
});

CPPSPEC_SPEC(event_buffer)
