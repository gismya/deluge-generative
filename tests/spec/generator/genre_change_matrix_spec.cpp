// The genre-change matrix (spec §21.2): every ordered pair of built-in genres changes in each of the three transition
// styles, through the real lifecycle (transition_piece.h). A piece of the first genre plays its default Recipe from
// its first Groove, confirms a change to the second, and plays until the incoming roster's first phrase ends. A genre
// added to allGenres() joins the matrix with no edit here.

#include "transition_piece.h"

#include "cppspec.hpp"

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <map>
#include <string>
#include <tuple>
#include <vector>

using namespace harness;

using Transition = TransitionPiece; ///< Through the real lifecycle: confirmed, and loaded at once.

namespace {

constexpr TransitionStyle kStyles[] = {TransitionStyle::BuildIntoDrop, TransitionStyle::Fade, TransitionStyle::Cut};

const char* styleName(TransitionStyle style) {
	switch (style) {
	case TransitionStyle::BuildIntoDrop:
		return "build";
	case TransitionStyle::Fade:
		return "fade";
	case TransitionStyle::Cut:
		return "cut";
	}
	return "?";
}

/// What each check found wrong in one case, one line per fault; empty when it passed.
struct Faults {
	std::string notesEnd;
	std::string inScaleAndRegister;
	std::string noOverflow;
	std::string tempoAndSwing;
	std::string rosterStart;
};

/// The fixed entropies the two Recipes roll from: different, so the two pieces' tempos mostly differ. The incoming
/// Recipe leans to the outgoing key, as the firmware's genre change does.
constexpr uint32_t kOutEntropy = 0;
constexpr uint32_t kInEntropy = 1;
/// The step a tick before whose line the change is confirmed: early in the outgoing piece's first Groove phrase.
constexpr int32_t kConfirmStep = 3;
/// The Glide setting each case is confirmed with: two bars, so a cut's glide starts well inside the outgoing piece's
/// first phrase and can't pass for the default bar.
constexpr int32_t kCutGlide = 2 * kBarTicks;

void fault(std::string& into, const std::string& label, const std::string& what) {
	into += label + ": " + what + "\n";
}

/// Every outgoing note starts before the start step and ends by it; the outgoing instance drains. No lean past the
/// start step is allowed: the outgoing roster decides nothing from it on.
void checkNotesEnd(const Transition& t, const std::string& label, std::string& into) {
	using NoteKey = std::tuple<uint8_t, Lane, uint8_t>;
	std::map<NoteKey, int32_t> sounding;
	for (const Played& p : t.sink.played) {
		if (p.incoming) {
			continue;
		}
		const NoteKey key{p.event.part, p.event.lane, p.event.pitch};
		if (p.event.kind == EventKind::NoteOn) {
			if (p.at >= t.sink.startAt) {
				fault(into, label, "an outgoing note-on at " + std::to_string(p.at) + ", past the start step");
			}
			sounding[key]++;
		}
		else if (p.event.kind == EventKind::NoteOff) {
			if (sounding[key]-- <= 0) {
				fault(into, label, "a stray outgoing note-off at " + std::to_string(p.at));
			}
			else if (p.at > t.sink.startAt) {
				fault(into, label, "an outgoing note-off at " + std::to_string(p.at) + ", past the start step");
			}
		}
	}
	for (const auto& [key, count] : sounding) {
		if (count > 0) {
			fault(into, label, "an outgoing note never ends, part " + std::to_string(std::get<0>(key)));
		}
	}
	if (t.sink.drained != 1 || t.driver.outgoing() != nullptr) {
		fault(into, label, "the outgoing instance didn't drain");
	}
}

/// Every incoming pitched note is in the incoming Recipe's scale and in its part's Register.
void checkScaleAndRegister(const Transition& t, const Recipe& in, const std::string& label, std::string& into) {
	int32_t pitched = 0;
	for (const Played& p : t.sink.from(true, EventKind::NoteOn)) {
		if (p.event.lane != Lane::None) {
			continue;
		}
		pitched++;
		const Register reg = in.genre->parts[p.event.part].registerOrDefault();
		const int32_t tonic = tonicNote(reg, in.keyRoot);
		const int32_t pitch = p.event.pitch;
		if (!inScale(pitch, in.keyRoot, in.scale)) {
			fault(into, label, "note " + std::to_string(pitch) + " out of the scale");
		}
		if (pitch < tonic + reg.spanLow || pitch > tonic + reg.spanHigh) {
			fault(into, label,
			      "note " + std::to_string(pitch) + " outside part " + std::to_string(p.event.part) + "'s Register");
		}
	}
	if (pitched == 0) {
		fault(into, label, "no incoming pitched note");
	}
}

void checkNoOverflow(const Transition& t, const Generator& incoming, const std::string& label, std::string& into) {
	for (const Generator* g : {static_cast<const Generator*>(t.out), &incoming}) {
		if (g->droppedEvents() != 0 || g->evictedNotes() != 0) {
			fault(into, label,
			      std::to_string(g->droppedEvents()) + " dropped events, " + std::to_string(g->evictedNotes())
			          + " evicted notes");
		}
	}
}

/// The tempo glides from the outgoing Recipe's to the incoming one's, each write a step nearer it, arriving on the
/// start step; a cut's glide starts the Glide setting before the cut line. Swing switches on the start step, in one
/// step.
void checkTempoAndSwing(const Transition& t, const Recipe& out, const Recipe& in, TransitionStyle style,
                        const std::string& label, std::string& into) {
	const auto& tempos = t.sink.tempos;
	if (out.tempo != in.tempo) {
		if (tempos.empty() || tempos.back().first != t.sink.startAt || tempos.back().second != in.tempo) {
			fault(into, label, "the tempo glide doesn't arrive at " + std::to_string(in.tempo) + " on the start step");
		}
		int32_t last = out.tempo;
		for (const auto& [at, tempo] : tempos) {
			if (tempo == last) {
				// A repeated write moves nothing: PerformDriver::setTempo() hands the sink every glide step it sets,
				// even one an Ambient glide rounds to the tempo it already wrote (writeTempo()'s own check sees only
				// no-ops).
				continue;
			}
			const bool towards =
			    in.tempo > out.tempo ? tempo > last && tempo <= in.tempo : tempo < last && tempo >= in.tempo;
			if (!towards || at > t.sink.startAt) {
				fault(into, label, "tempo " + std::to_string(tempo) + " at " + std::to_string(at) + " off the glide");
			}
			last = tempo;
		}
		if (style == TransitionStyle::Cut && !tempos.empty()) {
			// The first write is where the glide has moved the tempo by one unit: under a tick per unit of change
			// after the glide starts.
			// A glide longer than the time from the confirm to the cut line starts at the confirm instead: every case
			// confirms at kConfirmStep, well over the Glide setting before its cut line, so this only guards the check.
			const int32_t glide = t.lifecycle.glide;
			const int32_t starts = std::max(t.sink.startAt - glide, stepTick(kConfirmStep) - 1);
			const int32_t firstChange = glide / std::abs(in.tempo - out.tempo) + 1;
			const int32_t first = tempos.front().first;
			if (first < starts || first > starts + firstChange) {
				fault(into, label,
				      "the cut's glide starts " + std::to_string(t.sink.startAt - first)
				          + " ticks before the cut line, not " + std::to_string(glide));
			}
		}
	}
	else if (!tempos.empty()) {
		fault(into, label, "the tempo moves between two Recipes at one tempo");
	}
	const auto& swings = t.sink.swings;
	const size_t expected = out.swing != in.swing ? 1 : 0;
	if (swings.size() > 1 || swings.size() < expected) {
		fault(into, label, std::to_string(swings.size()) + " swing changes");
	}
	for (const auto& [at, swing] : swings) {
		if (at != t.sink.startAt || swing != in.swing) {
			fault(into, label,
			      "swing " + std::to_string(swing) + " at " + std::to_string(at) + ", not on the start step");
		}
	}
}

/// The incoming roster starts on a Drop (BUILD INTO A DROP), at the seam with its Intro (FADE THROUGH), or in Groove
/// on the cut line, the outgoing genre's next phrase line (HARD CUT AT PHRASE).
void checkRosterStart(const Transition& t, const Genre& from, const Genre& to, TransitionStyle style,
                      const std::string& label, std::string& into) {
	const Played first = t.sink.first(true, EventKind::Section);
	ArcPhrase firstPhrase;
	const bool known = t.driver.history().at(0, firstPhrase);
	if (first.at != t.sink.startAt) {
		fault(into, label, "the incoming roster's first Section isn't on the start step");
	}
	switch (style) {
	case TransitionStyle::BuildIntoDrop:
		if (first.event.section != SectionKind::Groove || !known || !firstPhrase.drop) {
			fault(into, label, "the incoming roster doesn't start on a Drop");
		}
		if (t.sink.last(false, EventKind::Section).event.section != SectionKind::Build) {
			fault(into, label, "the outgoing roster doesn't end in a Build");
		}
		break;
	case TransitionStyle::Fade: {
		const SectionKind opens = to.arc.introPhrases > 0 ? SectionKind::Intro : SectionKind::Groove;
		if (first.event.section != opens) {
			fault(into, label, "the incoming roster doesn't open with its Intro at the seam");
		}
		if (t.sink.last(false, EventKind::Section).event.section != SectionKind::Outro) {
			fault(into, label, "the outgoing roster doesn't end in an Outro");
		}
		break;
	}
	case TransitionStyle::Cut:
		if (first.event.section != SectionKind::Groove || !known || firstPhrase.drop) {
			fault(into, label, "the incoming roster doesn't cut in in Groove");
		}
		if (t.sink.startAt % stepTick(from.phraseSteps()) != 0) {
			fault(into, label, "the cut isn't on the outgoing genre's phrase line");
		}
		break;
	}
}

/// One case: `from`'s default Recipe changes to `to`'s in `style`, played until the incoming roster's first phrase
/// ends. @return what each check found wrong.
Faults playCase(const Genre& from, const Genre& to, TransitionStyle style) {
	const std::string label = std::string(from.name) + " -> " + to.name + " (" + styleName(style) + ")";
	const Recipe out = newRecipe(from, kOutEntropy);
	const Recipe in = newRecipe(to, kInEntropy, Key{out.keyRoot, out.scale}); // as the firmware's genre change
	Transition t(out, in);
	t.confirmAt(kConfirmStep, style, kCutGlide);
	Faults faults;
	if (!t.hook.runUntil([&] { return t.sink.starts > 0; })) {
		fault(faults.rosterStart, label, "the transition never starts");
		return faults;
	}
	if (t.sink.starts != 1) {
		fault(faults.rosterStart, label, "the transition starts " + std::to_string(t.sink.starts) + " times");
		return faults;
	}
	Generator& incoming = t.lifecycle.current();
	t.runSteps(to.phraseSteps());
	checkNotesEnd(t, label, faults.notesEnd);
	checkScaleAndRegister(t, in, label, faults.inScaleAndRegister);
	checkNoOverflow(t, incoming, label, faults.noOverflow);
	checkTempoAndSwing(t, out, in, style, label, faults.tempoAndSwing);
	checkRosterStart(t, from, to, style, label, faults.rosterStart);
	return faults;
}

/// Every case, played once on first use: the checks below each read their own part.
const std::vector<Faults>& matrix() {
	static const std::vector<Faults> cases = [] {
		std::vector<Faults> out;
		for (const Genre* from : allGenres()) {
			for (const Genre* to : allGenres()) {
				if (from == to) {
					continue;
				}
				for (TransitionStyle style : kStyles) {
					try {
						out.push_back(playCase(*from, *to, style));
					} catch (const std::exception& e) {
						// A throw in one case (a harness bound, say) fails that case by name, not the whole spec.
						Faults faults;
						fault(faults.rosterStart,
						      std::string(from->name) + " -> " + to->name + " (" + styleName(style) + ")",
						      std::string("threw: ") + e.what());
						out.push_back(faults);
					}
				}
			}
		}
		return out;
	}();
	return cases;
}

/// Every case's faults for one check, joined.
std::string all(std::string Faults::* check) {
	std::string out;
	for (const Faults& f : matrix()) {
		out += f.*check;
	}
	return out;
}

} // namespace

// clang-format off
describe genre_change_matrix("The genre-change matrix", $ {
	it("plays every ordered pair of built-in genres in each of the three styles", _ {
		const size_t genres = allGenres().size();
		expect(genres).to_be_greater_than(size_t{1});
		expect(matrix().size()).to_equal(genres * (genres - 1) * std::size(kStyles));
	});

	it("ends every outgoing note by the start step and drains the outgoing instance", _ {
		expect(all(&Faults::notesEnd)).to_equal(std::string());
	});

	it("plays every incoming pitched note in the incoming scale and its part's Register", _ {
		expect(all(&Faults::inScaleAndRegister)).to_equal(std::string());
	});

	it("never overflows a step's event buffer or a sounding-note table", _ {
		expect(all(&Faults::noOverflow)).to_equal(std::string());
	});

	it("glides the tempo to arrive on the start step, and switches swing there in one step", _ {
		expect(all(&Faults::tempoAndSwing)).to_equal(std::string());
	});

	it("starts the incoming roster on a Drop, at the seam with its Intro, or in Groove on the cut line", _ {
		expect(all(&Faults::rosterStart)).to_equal(std::string());
	});
});
// clang-format on

CPPSPEC_SPEC(genre_change_matrix)
