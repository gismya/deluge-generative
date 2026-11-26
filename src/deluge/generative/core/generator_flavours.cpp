/*
 * Copyright © 2026 Lars Johansson
 *
 * This file is part of The Synthstrom Audible Deluge Firmware.
 *
 * The Synthstrom Audible Deluge Firmware is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

// The Generator's pitched flavours: Loop lines and Answers, Sustain, Stab, Arp and Motif (spec §5.2, §8.1).

#include "generative/core/generator.h"
#include "generative/core/motion_curve.h"
#include "generative/core/tiers.h"
#include "generative/core/voicing.h"
#include <algorithm>

namespace deluge::generative {

// ---- Loop lines (spec §5.2) ----

namespace {

DrawKey lineKey(uint32_t seed, uint8_t part, uint8_t take, Purpose purpose, const PatternState& pick, uint16_t passes,
                int32_t lineStep, int32_t field) {
	return onPick(partKey(seed, part, Lane::None, take, purpose), pick, passes,
	              fieldWord(static_cast<uint32_t>(lineStep), field));
}

/// Fields of one line step's draws.
enum LineField : int32_t { kDegree, kOctaveUp, kOctaveDown, kAccent, kSlide, kMutate };
/// Fields of an Answer's draws: the tier's chance (0, the key as made), which transform, and a shift's way.
enum AnswerField : int32_t { kAnswerTier, kAnswerWhich, kAnswerWay };

/// One line step's value, octave and accent, drawn from `values` within the live Complexity's leap range and with its
/// octave-jump chances (spec §8.1): a Loop step's scale step from the chord root, or a Motif slot's chord position.
/// With no value in range, the step stays on 0, the root.
GEN_STEP_CODE LineStep drawLineStep(std::span<const LineWeight> values, const LineDraws& draws, DrawKey key,
                                    int32_t complexity) {
	LineStep out;
	const int32_t leap = draws.leap.at(complexity);
	const int32_t value = drawWeightedIf(withField(key, kDegree), values,
	                                     [leap](const LineWeight& v) { return v.value <= leap && -v.value <= leap; });
	if (value >= 0) {
		out.degree = values[value].value;
	}
	if (drawChance(withField(key, kOctaveUp), draws.octaveUp.at(complexity))) {
		out.octave = 1;
	}
	else if (drawChance(withField(key, kOctaveDown), draws.octaveDown.at(complexity))) {
		out.octave = -1;
	}
	out.accent = drawChance(withField(key, kAccent), draws.accent);
	return out;
}

/// One step of `spec`'s line, step `lineStep` of its pattern, rolled from the live Complexity (spec §5.2, §8.1): a
/// Motif slot stores a chord position; a Loop step stores a scale step and adds its slide. A Loop step off its jump
/// mask keeps the octave it drew at 0, after the draws, so the mask moves no other draw.
GEN_STEP_CODE LineStep rollLineStep(const PartSpec& spec, DrawKey key, int32_t complexity, int32_t lineStep) {
	if (spec.flavour == Flavour::Motif) {
		return drawLineStep(spec.motif.positions, spec.motif.draws, key, complexity);
	}
	LineStep out = drawLineStep(spec.loop.degrees, spec.loop.draws, key, complexity);
	out.slide = drawChance(withField(key, kSlide), spec.loop.slide.at(complexity));
	if (spec.loop.jumpSteps != 0 && (spec.loop.jumpSteps & (1U << (lineStep % kStepsPerBar))) == 0) {
		out.octave = 0;
	}
	return out;
}

} // namespace

GEN_STEP_CODE void Generator::rollLine(int32_t partIndex, int32_t complexity) {
	const PartSpec& spec = genre_->parts[partIndex];
	PartState& part = parts_[partIndex];
	part.passes = 0;
	const Pattern* pattern = spec.flavour == Flavour::Loop && spec.loop.rootOnAnchors && part.lanes[0].entry >= 0
	                             ? &spec.pool[part.lanes[0].entry]
	                             : nullptr;
	int32_t patternStep = 0;
	for (int32_t i = 0; i < kMaxPatternSteps; i++) {
		part.line[i] = rollLineStep(spec,
		                            lineKey(seed_, static_cast<uint8_t>(partIndex), takes_.take[partIndex],
		                                    Purpose::LineRoll, part.lanes[0], 0, i, 0),
		                            complexity, i);
		if (pattern != nullptr) {
			if (pattern->importance[patternStep] == kAnchor) {
				part.line[i].degree = 0;
				part.line[i].octave = 0;
			}
			if (++patternStep == pattern->length) {
				patternStep = 0;
			}
		}
	}
}

GEN_STEP_CODE void Generator::mutateLine(int32_t partIndex, int32_t motion, int32_t complexity) {
	const PartSpec& spec = genre_->parts[partIndex];
	PartState& part = parts_[partIndex];
	part.passes++;
	// The genre's chance at Motion 25, scaled by the flavour's curve: never at Motion 0 (spec §3.5).
	const bool motif = spec.flavour == Flavour::Motif;
	const int32_t chance = motif ? scaleChance(spec.motif.reroll, curves::motifReroll, motion)
	                             : scaleChance(spec.loop.mutation, curves::loopMutation, motion);
	const Pattern& pattern = spec.pool[part.lanes[0].entry];
	for (int32_t i = 0; i < pattern.length; i++) {
		if (pattern.importance[i] == kAnchor) {
			continue; // anchors never move
		}
		DrawKey key = lineKey(seed_, static_cast<uint8_t>(partIndex), takes_.take[partIndex], Purpose::LineMutation,
		                      part.lanes[0], part.passes, i, kMutate);
		if (!drawChance(key, chance)) {
			continue;
		}
		// Drawn the way a fresh pick rolls it. A Motif slot re-rolls its chord position and octave and keeps its
		// accent (spec §5.2).
		const bool accent = part.line[i].accent;
		part.line[i] = rollLineStep(spec, key, complexity, i);
		if (motif) {
			part.line[i].accent = accent;
		}
	}
}

GEN_STEP_CODE void Generator::updateLine(int32_t partIndex, PickChange change, const Step& step) {
	switch (change) {
	case PickChange::NewPick:
		rollLine(partIndex, step.complexity); // a new pick, a Complexity tier change included, rolls the line fresh
		break;
	case PickChange::Wrap:
		if (partFrozen(partIndex)) {
			parts_[partIndex].passes++; // a frozen line holds, and its passes count on (spec §6.7)
		}
		else {
			mutateLine(partIndex, step.motion, step.complexity); // the end of a pass
		}
		break;
	case PickChange::None:
		break;
	}
}

namespace {

/// Bars from one Answer to the next: N at Motion 25 divided by the Answers curve's multiple, to the nearest bar and at
/// least 1. 0 at Motion 0, which never answers (spec §3.5).
int32_t answerPeriod(int32_t bars, int32_t motion) {
	const int32_t multiple = motionMultiple(curves::answers, motion);
	if (multiple == 0) {
		return 0;
	}
	const int32_t period = (bars * kCurveOne + multiple / 2) / multiple;
	return period > 0 ? period : 1;
}

} // namespace

GEN_STEP_CODE int32_t Generator::heldStep(int32_t partIndex, int32_t step) const {
	return partFrozen(partIndex) ? parts_[partIndex].frozenPhraseStart + step % genre_->phraseSteps() : step;
}

void Generator::thaw(int32_t partIndex) {
	PartState& part = parts_[partIndex];
	for (PatternState& state : part.lanes) {
		if (state.hasWaiting) {
			// It jumps to where its counters are now: that pick's pattern, and a Loop's line rolled fresh with it.
			state.pick = state.waiting;
			state.entry = -1;
			state.hasWaiting = false;
			// The bar's Answer was drawn from the old pick. An Answer starts only on a bar line, so the rest of the bar
			// plays the new line's call.
			part.answer = AnswerKind::None;
		}
	}
}

GEN_STEP_CODE void Generator::updateAnswer(int32_t partIndex, const Step& step) {
	const PartSpec& spec = genre_->parts[partIndex];
	PartState& part = parts_[partIndex];
	part.answer = AnswerKind::None;
	const std::span<const Answer> answers = spec.loop.answers;
	const int32_t period = answerPeriod(spec.loop.answerBars, step.motion);
	// An automatic fill is a drum change known a bar or more ahead: the bar before it answers too (spec §5.2). A
	// frozen part answers there as well: the fill is the Arc's, acting on top like a section rule.
	const bool beforeFill = barBeforeAutoFill();
	if (answers.empty() || (period == 0 && !beforeFill)) {
		return;
	}
	// The last bar of every `period` bars from the pick answers.
	const PatternState& pick = part.lanes[0];
	const auto bars = static_cast<uint16_t>(heldStep(partIndex, step.index) / kStepsPerBar - pick.pick.bar);
	if (!beforeFill && (bars + 1) % period != 0) {
		return;
	}
	// One draw per Answer, here at its first step, from the live Complexity: keyed on the take, the pick and the bars
	// since it, so a take you return to replays its Answers.
	const DrawKey key =
	    onPick(partKey(seed_, static_cast<uint8_t>(partIndex), Lane::None, takes_.take[partIndex], Purpose::Answer),
	           pick, bars, 0);
	// The band runs from the list's lowest tier to its highest, and Complexity maps onto it as onto a pattern's.
	TierBand band{kMaxTier, 0};
	for (const Answer& answer : answers) {
		band.low = answer.tier < band.low ? answer.tier : band.low;
		band.high = answer.tier > band.high ? answer.tier : band.high;
	}
	const int32_t tier = playingTier(answers, bandTier(band, step.complexity, key));
	part.answer = answers[drawInTier(withField(key, kAnswerWhich), answers, tier)].kind;
	part.answerWay = drawBelow(withField(key, kAnswerWay), 2) == 0 ? 1 : -1;
}

bool Generator::linePitch(int32_t partIndex, const Pattern& pattern, int32_t patternStep, LinePitch& out) const {
	const PartState& part = parts_[partIndex];
	const int32_t length = pattern.length;
	const AnswerKind answer = part.answer;
	const bool answering = answer != AnswerKind::None;

	// Reverse and rotate reorder pitch content only: the rhythm stays the pattern's. Reverse counts the pattern's
	// onsets before Density thinning: the k-th of n plays onset n - 1 - k's content.
	int32_t from = patternStep;
	if (answering && answer == AnswerKind::Reverse) {
		int32_t k = 0;
		int32_t n = 0;
		for (int32_t i = 0; i < length; i++) {
			if (pattern.importance[i] > 0) {
				k += i < patternStep ? 1 : 0;
				n++;
			}
		}
		for (int32_t i = 0, onset = 0; i < length; i++) {
			if (pattern.importance[i] > 0 && onset++ == n - 1 - k) {
				from = i;
				break;
			}
		}
	}
	else if (answering && answer == AnswerKind::Rotate) {
		from = (patternStep - kStepsPerBeat + length) % length;
	}
	const LineStep& source = part.line[from];
	out = {source.degree + kDegreesPerScale * source.octave, source.accent, source.slide};
	if (!answering) {
		return true;
	}

	const bool anchor = pattern.importance[patternStep] == kAnchor;
	switch (answer) {
	case AnswerKind::Sparse:
		return anchor || patternStep % 2 == 0;
	case AnswerKind::AccentsOnly:
		return anchor || out.accent || pattern.level[patternStep] == VelocityLevel::Accent;
	case AnswerKind::Octave:
		out.steps += part.answerWay * kDegreesPerScale;
		break;
	case AnswerKind::Fifth:
		out.steps += part.answerWay * 4;
		break;
	case AnswerKind::Third:
		out.steps += part.answerWay * 2;
		break;
	case AnswerKind::Mirror: {
		// About the middle of the line's own range: the steps its pattern has onsets on.
		int32_t low = INT32_MAX;
		int32_t high = INT32_MIN;
		for (int32_t i = 0; i < length; i++) {
			if (pattern.importance[i] > 0) {
				const int32_t steps = part.line[i].degree + kDegreesPerScale * part.line[i].octave;
				low = steps < low ? steps : low;
				high = steps > high ? steps : high;
			}
		}
		out.steps = low + high - out.steps;
		break;
	}
	case AnswerKind::None:
	case AnswerKind::Reverse:
	case AnswerKind::Rotate:
		break;
	}
	return true;
}

uint8_t Generator::resolvePitch(int32_t partIndex, int32_t steps) const {
	// Flavour step and Answers → snap → Register fold → absolute note (spec §7.5). In a narrow Register an octave
	// Answer moves only the notes whose pitch class fits twice: the fold takes the rest back to where they were.
	const int32_t root = chordRoot();
	steps = snap(root + steps, genre_->parts[partIndex].degreeSet, recipe_.scale, root);
	return noteFor(steps, key(), partRegister(partIndex));
}

GEN_STEP_CODE void Generator::decideLoop(int32_t partIndex, const Step& step) {
	const PartSpec& spec = genre_->parts[partIndex];
	PartState& part = parts_[partIndex];
	PatternState& state = part.lanes[0];
	if (spec.pool.empty()) {
		return;
	}
	updateLine(partIndex, updatePick(state, spec.pool, spec.band, static_cast<uint8_t>(partIndex), Lane::None, step),
	           step);
	if (step.index % kStepsPerBar == 0) {
		updateAnswer(partIndex, step);
	}
	const Pattern& pattern = spec.pool[state.entry];
	int32_t patternStep = step.index % pattern.length;
	LinePitch pitch;
	if (!planned(partIndex, -1, step.index, step.sharedDensity) || keptOut(step, partIndex, Lane::None)
	    || !linePitch(partIndex, pattern, patternStep, pitch)) {
		return;
	}
	// A slid note runs into the next step, so a LEGATO voice glides to it; a note after a gap starts fresh (§8.6).
	VelocityLevel level = pitch.accent ? VelocityLevel::Accent : pattern.level[patternStep];
	int32_t length = pitch.slide ? kTicksPerStep + spec.loop.slideOverlapTicks : spec.loop.gateTicks;
	const uint8_t notePitch = resolvePitch(partIndex, pitch.steps);
	const bool muted = !mutes_.audible(partIndex);
	if (loopTies(partIndex, spec.loop, pitch.slide, level, muted, step)
	    && sounding_.extend(partIndex, Lane::None, notePitch, step.tick - 1, step.tick + length)) {
		part.loopOn = step.tick; // its last segment: Avoid measures the plain gate from here (spec §5.5)
		return;
	}
	part.loopOn = express(partIndex, spec.expression, state, patternStep,
	                      {Lane::None, notePitch, level, length, part.slid}, step);
	part.slid = pitch.slide;
	part.loopMuted = muted;
}

GEN_STEP_CODE bool Generator::loopTies(int32_t partIndex, const LoopSpec& loop, bool slide, VelocityLevel level,
                                       bool muted, const Step& step) const {
	// Only an unaccented plain note after a plain one ties, and only under the mute its note was struck with, so a
	// mute or unmute is heard at the next onset. An accent, the line's or the pattern's, is struck. A rest, a Section
	// line or another cut ended the sounding note: extend() finds none.
	if (loop.tieChance == 0 || slide || level == VelocityLevel::Accent || parts_[partIndex].slid
	    || parts_[partIndex].loopMuted != muted) {
		return false;
	}
	DrawKey key = partKey(seed_, static_cast<uint8_t>(partIndex), Lane::None, takes_.take[partIndex], Purpose::LoopTie);
	key.position[0] = static_cast<uint32_t>(heldStep(partIndex, step.index)); // freeze replays its phrase
	return drawChance(key, loop.tieChance);
}

// ---- Sustain (spec §5.2, §8.1) ----

GEN_STEP_CODE void Generator::decideSustain(int32_t partIndex, const Step& step) {
	if (arc_.stepInBar() != 0) {
		return; // a voicing changes only on bar lines: its notes last to the next one
	}
	const PartSpec& spec = genre_->parts[partIndex];
	PartState& part = parts_[partIndex];
	PartState::Sustain& state = part.sustain;
	const Register reg = partRegister(partIndex);
	// Whether an open voicing keeps its shared notes in place on this line, or may move one an octave (spec §5.2).
	bool holdShared = false;
	if (spec.sustain.holdSharedChance != 0) {
		DrawKey holdKey =
		    partKey(seed_, static_cast<uint8_t>(partIndex), Lane::None, takes_.take[partIndex], Purpose::HoldShared);
		holdKey.position[0] = static_cast<uint32_t>(arc_.section()) << 16 | arc_.occurrence();
		holdKey.position[1] = static_cast<uint32_t>(arc_.phraseInSection());
		holdKey.position[2] = static_cast<uint32_t>(arc_.barInPhrase());
		holdShared = drawChance(holdKey, spec.sustain.holdSharedChance);
	}
	const Voicing voicing = spec.sustain.pedal ? pedalVoicing(key(), reg)
	                        : spec.sustain.openVoicing
	                            ? openVoicing(chord(), key(), reg, part.voicing.notes(), holdShared)
	                            : leadVoicing(chord(), key(), reg, part.voicing.notes(), spec.sustain.doubleRoot);

	// Density thins the voicing by each note's importance, read again on every bar line with the gate hysteresis
	// (spec §8.1). The jitter is fixed per place, so more Density only ever adds notes.
	const int32_t threshold = genre_->densityCurve.threshold(step.density);
	const int32_t keepThreshold =
	    genre_->densityCurve.threshold(std::min<int32_t>(step.density + kGateHysteresis, kMacroFull));
	uint8_t open = 0;
	for (int32_t place = 0; place < kVoicePlaces; place++) {
		const int32_t importance = kVoiceImportance[place];
		bool plays = importance == kAnchor
		             || (spec.sustain.anchorExtension && place == static_cast<int32_t>(VoicePlace::Extension));
		if (!plays) {
			DrawKey jitterKey = partKey(seed_, static_cast<uint8_t>(partIndex), Lane::None, takes_.take[partIndex],
			                            Purpose::VoiceJitter);
			jitterKey.position[0] = static_cast<uint32_t>(place);
			const int32_t value = jitteredImportance(jitterKey, importance);
			plays = (state.openPlaces & (1U << place)) != 0 ? value > keepThreshold : value >= threshold;
		}
		open |= plays ? static_cast<uint8_t>(1U << place) : 0;
	}

	// The whole voicing re-strikes at a chord change, on a retrigger line and at a mute change, so a muted Pad stops
	// sounding and an unmuted one comes back. A cut on the line (a Section line, a key change, a new sound) ended its
	// notes there, and extend() doesn't hold those on: they re-strike too (spec §5.2, §7.6, §10.7, §12.5). The one
	// exception, below: a part that holds across Section lines, whose notes the Section line left uncut.
	const int32_t retrigger = spec.sustain.retriggerBars;
	const int32_t phraseBars = genre_->phraseBars;
	bool restrike = state.muted != !mutes_.audible(partIndex);
	if (retrigger > 0) {
		restrike = restrike
		           || (retrigger <= phraseBars
		                   ? arc_.barInPhrase() % retrigger == 0
		                   : arc_.barInPhrase() == 0 && arc_.phraseInSection() % (retrigger / phraseBars) == 0);
	}
	// A part that holds across Section lines keeps, on a Section line, the pitches its new voicing shares with the
	// old one, and one that holds common tones keeps them on any other line: only the changed ones end and strike
	// there. The first Groove's first line re-strikes a holding part once, so a piece begun in an Intro plays at the
	// Groove's level from there on (spec §5.2).
	const bool firstGrooveLine = step.sectionStart && arc_.section() == SectionKind::Groove && arc_.occurrence() == 0;
	const bool holdsHere =
	    step.sectionStart ? spec.sustain.holdsAcrossSections && !firstGrooveLine : spec.sustain.holdsCommonTones;
	restrike = restrike || (spec.sustain.holdsAcrossSections && firstGrooveLine);
	restrike = restrike || (!holdsHere && !voicing.sameNotes(part.voicing));
	part.voicing = voicing;
	state.openPlaces = open;
	state.muted = !mutes_.audible(partIndex);

	// A closed gate or a section rule: the notes end on this line, and a gate opening on a later one brings the
	// voicing in there, not at the next chord (spec §5.6).
	if (!gateOpen(partIndex, Lane::None) || keptOut(step, partIndex, Lane::None)) {
		return;
	}
	const int32_t end = step.tick + kTicksPerBar;
	const int32_t base = kLevelVelocity[static_cast<int32_t>(VelocityLevel::Normal)];
	for (int32_t i = 0; i < voicing.size; i++) {
		if ((open & (1U << static_cast<uint32_t>(voicing.place[i]))) == 0) {
			continue; // thinned out: a sounding note ends on this line
		}
		// Sounding up to this line, and not cut on it, it holds on to the next. One a Rest, the Gap or a cut ended
		// earlier, or a cut ended on the line, comes in again.
		if (!restrike && sounding_.extend(partIndex, Lane::None, voicing.pitch[i], step.tick - 1, end)) {
			continue;
		}
		noteOn(partIndex,
		       {Lane::None, voicing.pitch[i], static_cast<int16_t>(base), velocity(base, step.energy), step.tick, end});
	}
}

// ---- Stab (spec §5.2, §8.1) ----

GEN_STEP_CODE Generator::PlayingStep Generator::playingStep(int32_t partIndex, const Step& step) {
	const PartSpec& spec = genre_->parts[partIndex];
	PatternState& state = parts_[partIndex].lanes[0];
	if (spec.pool.empty()) {
		return {};
	}
	const PickChange change =
	    updatePick(state, spec.pool, spec.band, static_cast<uint8_t>(partIndex), Lane::None, step);
	if (!planned(partIndex, -1, step.index, step.sharedDensity) || keptOut(step, partIndex, Lane::None)) {
		return {-1, change};
	}
	return {step.index % spec.pool[state.entry].length, change};
}

GEN_STEP_CODE void Generator::decideStab(int32_t partIndex, const Step& step) {
	const PartSpec& spec = genre_->parts[partIndex];
	PartState& part = parts_[partIndex];
	// The voicing changes only on bar lines, where chords change, voice-led from the whole voicing before, whether
	// or not an onset plays: so Density thinning never moves the next chord, and every stab of a chord is the same.
	if (arc_.stepInBar() == 0 || part.voicing.size == 0) {
		part.voicing = leadVoicing(chord(), key(), partRegister(partIndex), part.voicing.notes(), /*doubleRoot=*/false);
	}
	const int32_t patternStep = playingStep(partIndex, step).step;
	if (patternStep < 0) {
		return;
	}
	const Pattern& pattern = spec.pool[part.lanes[0].entry];
	// So it never overlaps the next stab, and the part's table holds an onset's notes plus the next's (spec §2.7).
	const int32_t length = onsetLength(pattern, patternStep, spec.stab.gateTicks, spec.expression);
	for (uint8_t pitch : part.voicing.notes()) {
		express(partIndex, spec.expression, part.lanes[0], patternStep,
		        {Lane::None, pitch, pattern.level[patternStep], length, false}, step);
	}
}

GEN_STEP_CODE int32_t Generator::onsetLength(const Pattern& pattern, int32_t patternStep, int32_t gate,
                                             const Expression& expression) const {
	return onsetTicks(pattern, patternStep, arc_.stepInBar(), gate, detail::leanOn(expression, patternStep));
}

// ---- Arp (spec §5.2, §8.1) ----

GEN_STEP_CODE void Generator::decideArp(int32_t partIndex, const Step& step) {
	const PartSpec& spec = genre_->parts[partIndex];
	const PatternState& state = parts_[partIndex].lanes[0];
	const int32_t patternStep = playingStep(partIndex, step).step;
	if (patternStep < 0) {
		return;
	}
	const Pattern& pattern = spec.pool[state.entry];
	// arpNotesValid() holds every built-in genre to a walk per pool entry of 1 to kMaxArpOctaves octaves.
	const ArpWalk walk = spec.arp.walks[state.entry];
	// The walk position: the pattern's onsets before this one in the pass, before Density thinning, so a thinned
	// note never shifts the rest and nothing carries over from the pass before (spec §8.1).
	int32_t position = 0;
	for (int32_t i = 0; i < patternStep; i++) {
		position += pattern.importance[i] > 0 ? 1 : 0;
	}
	const Chord current = chord();
	const int32_t notes = current.size * walk.octaves;
	int32_t index = position % notes;
	switch (walk.order) {
	case ArpOrder::Up:
		break;
	case ArpOrder::Down:
		index = notes - 1 - index;
		break;
	case ArpOrder::UpDown: {
		// Up, then down, turning on the top and bottom notes without playing them twice.
		const int32_t turn = notes > 1 ? 2 * notes - 2 : 1;
		index = position % turn;
		index = index < notes ? index : turn - index;
		break;
	}
	case ArpOrder::Pulse:
		// The root, then the root an octave up (its walk's first note on the second octave).
		index = (position & 1) != 0 ? current.size : 0;
		break;
	case ArpOrder::Random: {
		// One draw per onset, keyed on the pick and the step like Density's jitter, so a pass loops while the pick
		// holds, a take re-rolls it and freeze holds it.
		DrawKey key =
		    partKey(seed_, static_cast<uint8_t>(partIndex), Lane::None, takes_.take[partIndex], Purpose::ArpWalk);
		index = drawBelow(onPick(key, state, 0, static_cast<uint32_t>(patternStep)), notes);
		break;
	}
	}
	const uint8_t pitch = arpNote(current, index, key(), partRegister(partIndex), spec.degreeSet);
	express(partIndex, spec.expression, state, patternStep,
	        {Lane::None, pitch, pattern.level[patternStep],
	         onsetLength(pattern, patternStep, spec.arp.gateTicks, spec.expression), false},
	        step);
}

// ---- Motif (spec §5.2, §8.1) ----

GEN_STEP_CODE void Generator::decideMotif(int32_t partIndex, const Step& step) {
	const PartSpec& spec = genre_->parts[partIndex];
	const PartState& part = parts_[partIndex];
	const PlayingStep playing = playingStep(partIndex, step);
	// The line moves with the pick whether or not this step plays: a new pick rolls it, a pass end re-rolls slots.
	updateLine(partIndex, playing.change, step);
	const int32_t patternStep = playing.step;
	if (patternStep < 0) {
		return;
	}
	const Pattern& pattern = spec.pool[part.lanes[0].entry];
	// The slot stores a chord position, so it plays that position on whatever chord plays now.
	const LineStep& slot = part.line[patternStep];
	const uint8_t pitch = motifNote(chord(), slot.degree, slot.octave, key(), partRegister(partIndex), spec.degreeSet);
	const VelocityLevel level = slot.accent ? VelocityLevel::Accent : pattern.level[patternStep];
	express(partIndex, spec.expression, part.lanes[0], patternStep,
	        {Lane::None, pitch, level, onsetLength(pattern, patternStep, spec.motif.gateTicks, spec.expression), false},
	        step);
}

} // namespace deluge::generative
