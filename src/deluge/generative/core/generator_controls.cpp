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

// The Generator's sound controls: macro layers, gestures, Throws and drift (spec §11.3 to §11.5).

#include "generative/core/generator.h"
#include <algorithm>
#include <array>
#include <iterator>

namespace deluge::generative {

namespace {

/// setMoves() for every kind of set and control, worked out once: the tick reads it for each control of each set.
constexpr auto kSetMoves = [] {
	std::array<std::array<bool, kNumSoundControls>, kNumControlSetKinds> moves{};
	for (int32_t kind = 0; kind < kNumControlSetKinds; kind++) {
		for (int32_t c = 0; c < kNumSoundControls; c++) {
			moves[kind][c] = setMoves(static_cast<ControlSetKind>(kind), static_cast<SoundControl>(c));
		}
	}
	return moves;
}();

} // namespace

GEN_STEP_CODE int32_t Generator::macroLayer(SoundControl control) const {
	const auto c = static_cast<int32_t>(control);
	int32_t offset = 0;
	for (int32_t row = 0; row < kMacroLayerRows; row++) {
		if (kMacroLayerSigns[row][c] == 0) {
			continue;
		}
		const auto macro = static_cast<Macro>(row);
		// Effective, so Section offsets shade the sound with no rules of their own; measured from the genre's default,
		// where its sounds sit at home.
		const int32_t fromDefault = effectiveMacro(macro) - genre_->defaults[macro];
		offset += kMacroLayerSigns[row][c] * genre_->soundWeight[row][c] * fromDefault * kMacroLayerDepth
		          / (1000 * kMacroLayerSpan);
	}
	return offset;
}

GEN_STEP_CODE Generator::ThrowLine Generator::throwLine() const {
	// The line is near when the step just decided is at most kThrowWindowSteps before it. The Gap's place is fixed by
	// the Build's length; a Break's comes from a waiting force, or from the Groove ending on its own. Once a
	// transition has landed, the outgoing parts' last hits come before its Build's Gap or before the start step.
	const auto inWindow = [](int32_t steps) { return steps >= 1 && steps <= kThrowWindowSteps; };
	const bool leaving = stopAt_ != kNever;
	const int32_t gap = gapSteps(genre_->arc.gap);
	if (arc_.section() == SectionKind::Build && gap > 0 && arc_.sectionSteps() > gap) {
		return {inWindow(arc_.sectionSteps() - gap - arc_.stepInSection()),
		        leaving ? GestureMoment::ThrowTransition : GestureMoment::ThrowGap};
	}
	if (leaving) {
		return {inWindow(stopAt_ - nextStep_), GestureMoment::ThrowTransition};
	}
	// Where a waiting force, End or transition lands: whichever comes first starts there. A cut lands on its start
	// step.
	const Arc::Move move = waitingMove();
	const int32_t ownAt =
	    arc_.breakEndsPhrase(recipe_.macros[Macro::Motion]) ? arc_.stepsToPhraseLine() + 1 : INT32_MAX;
	if (move.at <= ownAt) {
		const bool throws = move.stops || move.section == SectionKind::Break;
		return {throws && inWindow(move.at), move.stops ? GestureMoment::ThrowTransition : GestureMoment::ThrowBreak};
	}
	return {inWindow(ownAt), GestureMoment::ThrowBreak};
}

GEN_STEP_CODE bool Generator::gestureActsOn(const ControlSet& set, Gesture gesture) const {
	const Lane aim = partOf(set).gestures.aim[static_cast<int32_t>(gesture)];
	switch (set.kind) {
	case ControlSetKind::KitWide:
		return aim == Lane::None;
	case ControlSetKind::KitLane:
		return aim == set.lane;
	default:
		return true; // FX lanes always use their own row controls (spec §11.4)
	}
}

bool Generator::silencedAt(const ControlSet& set, ThrowLine line) const {
	if (line.moment != GestureMoment::ThrowBreak) {
		return true; // no note starts in the Gap, nor in the outgoing instance after its start step
	}
	const Keeps keeps = genre_->arc.keepsIn(SectionKind::Break);
	if (set.kind != ControlSetKind::KitWide) {
		return !keeps.lets(set.part, set.lane);
	}
	return !anyLaneOf(set, [&](Lane lane) { return keeps.lets(set.part, lane); });
}

bool Generator::updateThrow(ControlSet& set, const ThrowLine& line, const PartsAndLanes& hits) {
	if (!gestureActsOn(set, Gesture::Throw)) {
		return false;
	}
	// The kit plays when any of its lanes does.
	const bool hit = hits.has(set.part, set.lane)
	                 || (set.kind == ControlSetKind::KitWide
	                     && anyLaneOf(set, [&](Lane lane) { return (hits.lanes & laneBit(lane)) != 0; }));
	// A line's Throw first (a Break, the Gap, a transition's start step): every hit in the window before it spikes,
	// whatever a drop-out holds, so a kit-wide Throw whose drop-out ends on the line spikes for it like any other.
	const bool lineRules = line.near && partOf(set).gestures.on(line.moment) && silencedAt(set, line);
	if (hit && lineRules) {
		set.thrown = 0; // this hit may be the last before the line
		return true;
	}
	// A drop-out's throw target (spec §8.8), with no switch (§11.4): a pitched part's set, or on Drums the set its
	// Throw acts on that holds the lane. Its own hits in the window before the drop-out spike, the hold lasts the whole
	// drop-out and the fall takes its last beat, so it returns dry. A Throw the drop-out holds sits at `hold` or past
	// it; one below that in a line's window was spiked again for the line, which then rules it as any Throw.
	const PartsAndLanes target = line.dropOutTarget;
	const bool targeted = !target.empty()
	                      && (set.kind == ControlSetKind::KitWide
	                              ? anyLaneOf(set, [&](Lane lane) { return (target.lanes & laneBit(lane)) != 0; })
	                              : (set.lane == Lane::None ? (target.parts & (1U << set.part)) != 0
	                                                        : (target.lanes & laneBit(set.lane)) != 0));
	if (targeted) {
		const int32_t hold = genre_->throwHoldSteps;
		if (line.dropOutNear && target.intersects(hits)) {
			set.thrown = 0;
			return true;
		}
		const bool held = set.thrown >= 0 && !(lineRules && set.thrown < hold);
		if (held && line.dropOutLeft > 0) {
			set.thrown = static_cast<int16_t>(hold + std::max<int32_t>(kThrowDecaySteps - line.dropOutLeft, 0));
			return false;
		}
		if (held && set.thrown >= hold && line.dropOutFalling) {
			// From a Section line that ended it early, the Throw falls over a beat whatever the part plays; at its own
			// end it has fallen already.
			set.thrown = static_cast<int16_t>(set.thrown + 1);
			if (set.thrown > hold + kThrowDecaySteps) {
				set.thrown = -1;
			}
			return false;
		}
	}
	if (set.thrown < 0) {
		return false;
	}
	if (hit) {
		set.thrown = -1; // it plays again: back at home
		return true;
	}
	set.thrown = static_cast<int16_t>(set.thrown + 1);
	if (set.thrown > genre_->throwHoldSteps + kThrowDecaySteps) {
		set.thrown = -1;
	}
	return false;
}

GEN_STEP_CODE int32_t Generator::gestureLayer(const ControlSet& set, SoundControl control, int32_t closeWash,
                                              int32_t rise) const {
	const GestureSpec& gestures = partOf(set).gestures;
	const auto scaled = [&](int32_t amount, Gesture gesture, int32_t level) {
		return amount * level * gestures.depth[static_cast<int32_t>(gesture)] / (1000 * 1000);
	};
	const bool outro = closeWashBy_ == SectionKind::Outro;
	const bool intro = arc_.section() == SectionKind::Intro;
	int32_t offset = 0;
	if (control == SoundControl::Brightness && closeWash > 0 && gestureActsOn(set, Gesture::Close)
	    && gestures.on(outro ? GestureMoment::CloseOutro : GestureMoment::CloseBreak)) {
		offset += scaled(kCloseBrightness, Gesture::Close, closeWash);
	}
	if (control == SoundControl::Space && closeWash > 0 && gestureActsOn(set, Gesture::Wash)
	    && gestures.on(outro ? GestureMoment::WashOutro : GestureMoment::WashBreak)) {
		offset += scaled(kWashSpace, Gesture::Wash, closeWash);
	}
	if (rise > 0 && gestureActsOn(set, Gesture::Rise)
	    && gestures.on(intro ? GestureMoment::RiseIntro : GestureMoment::RiseBuild)) {
		int32_t amount = 0;
		if (control == SoundControl::Thin) {
			amount = kRiseThin;
		}
		else if (control == SoundControl::Brightness) {
			amount = kRiseBrightness;
		}
		else if (control == SoundControl::Space && set.lane == Lane::Riser) {
			amount = kRiseRiserSpace;
		}
		offset += scaled(amount, Gesture::Rise, rise);
	}
	if (control == SoundControl::Space && set.thrown >= 0) {
		// Full while the hold lasts, then down over a beat.
		const int32_t hold = genre_->throwHoldSteps;
		const int32_t level =
		    set.thrown <= hold ? 1000 : (hold + kThrowDecaySteps - set.thrown) * 1000 / kThrowDecaySteps;
		offset += scaled(kThrowSpace, Gesture::Throw, level);
	}
	return offset;
}

GEN_STEP_CODE void Generator::driftLayer(const ControlSet& set, const Step& step, int32_t walked,
                                         int32_t (&offsets)[kNumSoundControls]) const {
	std::fill(std::begin(offsets), std::end(offsets), 0);
	const DriftSpec& drift = partOf(set).drift;
	const int32_t depth = step.motion * kDriftDepth * drift.weight / (kMacroFull * 1000);
	if (set.kind == ControlSetKind::KitWide || depth == 0) {
		return;
	}
	for (int32_t d = 0; d < kDriftingControls; d++) {
		const SoundControl control = kDrifting[d];
		const auto c = static_cast<int32_t>(control);
		if (control == SoundControl::Brightness && drift.sweepBars > 0) {
			// A triangle from -1000 up to 1000 and back over the period, from a seeded quarter, counted from step 0 so
			// it keeps to phrase lines.
			const int32_t period = drift.sweepBars * kStepsPerBar;
			const int32_t quarter = drawBelow(partKey(seed_, set.part, Lane::None, 0, Purpose::SweepPhase), 4);
			const int32_t at = (step.index + quarter * period / 4) % period;
			const int32_t triangle = at < period / 2 ? -1000 + 4000 * at / period : 3000 - 4000 * at / period;
			offsets[c] = triangle * depth / 1000;
			continue;
		}
		// The walk: from one seeded point to the next, eased in and out (updateWalk()).
		const int32_t from = set.walkFrom[d] * kWalkQuantum;
		const int32_t to = set.walkTo[d] * kWalkQuantum;
		offsets[c] = (from * 1000 + (to - from) * walked) * depth / (1000 * 1000);
	}
}

GEN_STEP_CODE int32_t Generator::updateWalk(ControlSet& set, uint32_t walkPoint, int32_t walkTowards) {
	if (set.kind == ControlSetKind::KitWide) {
		return 0;
	}
	// Each set sits at its own seeded place between the points, and each of its controls walks its own points.
	DrawKey key = partKey(seed_, set.part, set.lane, 0, Purpose::DriftPhase);
	const int32_t towards = walkTowards + drawBelow(key, kDriftApart);
	const uint32_t point = walkPoint + static_cast<uint32_t>(towards / kDriftApart);
	const auto pointBits = static_cast<uint16_t>(point);
	if (pointBits != set.walkPoint) {
		// Passed one point: the old `to` is the new `from`. Otherwise both are drawn (a first step; kNoWalk's index is
		// drawn afresh too, giving the same points).
		const bool passedOne =
		    set.walkPoint != ControlSet::kNoWalk && pointBits == static_cast<uint16_t>(set.walkPoint + 1);
		key.purpose = Purpose::DriftPoint;
		const auto drawPoint = [&](uint32_t at, int32_t control) {
			key.position[0] = at;
			key.position[1] = static_cast<uint32_t>(control);
			return static_cast<int8_t>(drawBelow(key, 2 * (1000 / kWalkQuantum) + 1) - 1000 / kWalkQuantum);
		};
		for (int32_t d = 0; d < kDriftingControls; d++) {
			const auto c = static_cast<int32_t>(kDrifting[d]);
			set.walkFrom[d] = passedOne ? set.walkTo[d] : drawPoint(point, c);
			set.walkTo[d] = drawPoint(point + 1, c);
		}
		set.walkPoint = pointBits;
	}
	const int32_t between = towards % kDriftApart * 1000 / kDriftApart; // per-mille of the way to the next point
	return between * between / 1000 * (3000 - 2 * between) / 1000;      // smoothstep, per-mille
}

int32_t Generator::closeWashLevel(int32_t step) const {
	return closeWashFrom_ == kNever ? 0
	                                : std::min(step - closeWashFrom_ + 1, kGestureEaseSteps) * 1000 / kGestureEaseSteps;
}

GEN_STEP_CODE int32_t Generator::riseLevel() const {
	const SectionKind section = arc_.section();
	if ((section != SectionKind::Build && section != SectionKind::Intro) || arc_.sectionSteps() <= 0) {
		return 0;
	}
	const int32_t x = std::min(arc_.stepInSection() + 1, arc_.sectionSteps()) * 1000 / arc_.sectionSteps();
	return x * x / 1000; // an ease-in, peaking on the Section's last step
}

bool Generator::throwsFalling() const {
	for (int32_t i = 0; i < controlCount_; i++) {
		if (controls_[i].thrown >= 0) {
			return true;
		}
	}
	return false;
}

void Generator::fallThrows() {
	// Every part is silent from the start step on, so each Throw holds out its hold and falls. The other gestures
	// stand as on the last decided step: the Arc stopped there. So do the macro layer and drift, kept from that step,
	// as a macro turn can land on the stop step: updateControls() moved the walk on by that step's Motion after
	// deciding it, and this steps it back, so a set's Space falls to where it stood and the walk draws no new points.
	static_assert(kMacroFull <= kDriftApart, "a step's Motion moves the walk past one point at most");
	uint32_t walkPoint = driftPoint_;
	int32_t walkTowards = driftTowards_ - driftLastMotion_;
	if (walkTowards < 0) {
		walkTowards += kDriftApart;
		walkPoint--;
	}
	const int32_t n = nextStep_;
	const int32_t last = n - 1;
	events_.beginStep();
	cutParts_ = 0;
	Step step{};
	step.index = last;
	step.motion = driftLastMotion_;
	const int32_t closeWash = closeWashLevel(last);
	const int32_t rise = riseLevel();
	const int32_t macro = lastSpaceMacro_;
	constexpr auto kSpace = static_cast<int32_t>(SoundControl::Space);
	for (int32_t i = 0; i < controlCount_; i++) {
		ControlSet& set = controls_[i];
		if (set.thrown < 0) {
			continue;
		}
		set.thrown = static_cast<int16_t>(set.thrown + 1);
		if (set.thrown > genre_->throwHoldSteps + kThrowDecaySteps) {
			set.thrown = -1;
		}
		int32_t drift[kNumSoundControls];
		driftLayer(set, step, updateWalk(set, walkPoint, walkTowards), drift);
		const int32_t sum = (set.kind == ControlSetKind::KitLane ? 0 : macro)
		                    + gestureLayer(set, SoundControl::Space, closeWash, rise) + drift[kSpace];
		const int32_t target = std::clamp<int32_t>(sum, -kControlFull, kControlFull);
		if (target != set.sent[kSpace]) {
			set.sent[kSpace] = static_cast<int8_t>(target);
			push(Event::controlChange(stepTick(n), set.part, set.lane, SoundControl::Space, set.sent[kSpace], false));
		}
	}
	nextStep_ = n + 1;
}

GEN_STEP_CODE void Generator::updateControls(const Step& step) {
	// Reset: every Groove's downbeat snaps the gestures back (the Drop, the first Groove after an Intro, a forced one).
	const bool reset = step.index > 0 && arc_.section() == SectionKind::Groove && arc_.sectionStartsHere();
	if (reset) {
		closeWashFrom_ = kNever;
		for (int32_t i = 0; i < controlCount_; i++) {
			controls_[i].thrown = -1;
		}
	}
	const SectionKind section = arc_.section();
	if (arc_.sectionStartsHere() && (section == SectionKind::Break || section == SectionKind::Outro)) {
		closeWashFrom_ = closeWashFrom_ == kNever ? step.index : closeWashFrom_;
		closeWashBy_ = section;
	}
	const int32_t closeWash = closeWashLevel(step.index);
	const int32_t rise = riseLevel();
	// The notes this step decided, muted ones too, so a mute never changes a Throw.
	PartsAndLanes hits;
	for (int32_t i = events_.stepStart(); i < events_.size(); i++) {
		const Event& e = events_.at(i);
		if (e.kind == EventKind::NoteOn) {
			hits.parts |= static_cast<uint8_t>(e.lane == Lane::None ? 1U << e.part : 0U);
			hits.lanes |= e.lane == Lane::None ? 0 : laneBit(e.lane);
		}
	}
	ThrowLine line = throwLine();
	if (dropOut_ >= 0) {
		line.dropOutTarget = genre_->dropOuts[dropOut_].throwTarget;
		line.dropOutNear = step.index < dropOutFrom_ && dropOutFrom_ - step.index <= kThrowWindowSteps;
		line.dropOutLeft = step.index >= dropOutFrom_ && step.index < dropOutTo_ ? dropOutTo_ - step.index : 0;
		line.dropOutFalling = step.index >= dropOutTo_ && step.index - dropOutTo_ < kThrowDecaySteps;
	}
	int32_t macro[kNumSoundControls];
	for (int32_t c = 0; c < kNumSoundControls; c++) {
		macro[c] = macroLayer(static_cast<SoundControl>(c));
	}

	for (int32_t i = 0; i < controlCount_; i++) {
		ControlSet& set = controls_[i];
		const bool spaceSnaps = updateThrow(set, line, hits);
		int32_t drift[kNumSoundControls];
		driftLayer(set, step, updateWalk(set, driftPoint_, driftTowards_), drift);
		const bool gesturing = closeWash > 0 || rise > 0 || set.thrown >= 0;
		const auto kind = static_cast<int32_t>(set.kind);
		for (int32_t c = 0; c < kNumSoundControls; c++) {
			const auto control = static_cast<SoundControl>(c);
			if (!kSetMoves[kind][c]) {
				continue;
			}
			// Macro + gesture + drift, summed and clamped to the control's range (spec §11.3). A Drums lane takes no
			// macro layer: that moves the kit-wide controls.
			const int32_t sum = (set.kind == ControlSetKind::KitLane ? 0 : macro[c])
			                    + (gesturing ? gestureLayer(set, control, closeWash, rise) : 0) + drift[c];
			const int32_t target = std::clamp<int32_t>(sum, -kControlFull, kControlFull);
			if (target == set.sent[c]) {
				continue;
			}
			set.sent[c] = static_cast<int8_t>(target);
			// The piece's first step jumps there, as does a Reset; a Throw jumps on its hit and back on the next.
			const bool snap = step.index == 0 || reset || (spaceSnaps && control == SoundControl::Space);
			push(Event::controlChange(step.tick, set.part, set.lane, control, set.sent[c], snap));
		}
	}

	// The walk moves on by Motion: a point a phrase at Motion 25 (spec §11.5).
	driftTowards_ += step.motion;
	driftPoint_ += static_cast<uint32_t>(driftTowards_ / kDriftApart);
	driftTowards_ %= kDriftApart;
	driftLastMotion_ = static_cast<int16_t>(step.motion);
	lastSpaceMacro_ = static_cast<int16_t>(macro[static_cast<int32_t>(SoundControl::Space)]);
}

} // namespace deluge::generative
