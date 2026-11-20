// PROTOTYPE (#414, throwaway): the curve candidates for Part bias (#303), for the owner to compare by ear and eye.
// Harness only: the Generator reads it through Generator::testBias_, under GENERATOR_TEST_SEAMS. #303 replaces this
// with the chosen curve and the real per-part state.

#pragma once

#include "generative/core/recipe.h"

#include <algorithm>
#include <cstdint>

namespace deluge::generative::prototype {

enum class BiasModel : uint8_t {
	Linear,          ///< #303's model 1: v + o, clamped 0..50.
	Curved,          ///< Model 3, headroom-proportional: o > 0: v + o/50·(50 − v); o < 0: v·(1 + o/50).
	CurvedGateShift, ///< Model 3 for everything but the Density gates, which see model 4: the gate moves by o.
	Knee,            ///< Hybrid: linear through the middle, easing into the end it heads for (kneeBiased()).
	Blend,           ///< Hybrid: the mean of Linear's and Curved's outputs.
};

/// The narrowest knee: a small bias still eases into its end over the last 10 points, instead of hitting it.
constexpr int32_t kKneeMin = 10 * kMacroUnitsPerPoint;

/// The knee hybrid. Linear (`v + o`) while the end the bias heads for is at least K away, with
/// K = min(50, max(10, 2·|o|)). Inside the knee the bias shrinks in step with the room left: o·d/K for d points from
/// the end, so the part still reaches that end exactly when the shared value does. K = 2·|o| keeps the part moving at
/// half the shared value's speed or more inside the knee (slope 1 − |o|/K ≥ ½), so it keeps following the Arc there,
/// where linear pins it. From |o| = 25 the knee spans the whole range and this is the curved model.
constexpr int32_t kneeBiased(int32_t shared, int32_t offset) {
	const int32_t o = std::clamp<int32_t>(offset, -kMacroFull, kMacroFull);
	const int32_t magnitude = o < 0 ? -o : o;
	const int32_t knee = std::min<int32_t>(kMacroFull, std::max<int32_t>(kKneeMin, 2 * magnitude));
	const int32_t room = o >= 0 ? kMacroFull - shared : shared; // the distance to the end the bias heads for
	if (room >= knee) {
		return shared + o;
	}
	// Rounded down, as the curved model rounds, so the two agree exactly from |o| = 25.
	if (o >= 0) {
		return shared + static_cast<int32_t>(int64_t{o} * room / knee);
	}
	return shared - static_cast<int32_t>((int64_t{magnitude} * room + knee - 1) / knee);
}

/// `shared`, the value every part plays at after the Arc (effectiveOn()), moved by the bias `offset`, both in macro
/// units. Within 0..kMacroFull.
constexpr int32_t biased(BiasModel model, int32_t shared, int32_t offset) {
	if (model == BiasModel::Linear) {
		return std::clamp<int32_t>(shared + offset, 0, kMacroFull);
	}
	if (model == BiasModel::Knee) {
		return kneeBiased(shared, offset);
	}
	if (model == BiasModel::Blend) {
		return (biased(BiasModel::Linear, shared, offset) + biased(BiasModel::Curved, shared, offset)) / 2;
	}
	const int32_t o = std::clamp<int32_t>(offset, -kMacroFull, kMacroFull);
	if (o >= 0) {
		return shared + static_cast<int32_t>(int64_t{o} * (kMacroFull - shared) / kMacroFull);
	}
	return static_cast<int32_t>(int64_t{shared} * (kMacroFull + o) / kMacroFull);
}

/// The Density a part's gates compare with their thresholds. Model 4 moves the threshold by the bias, so the gate sees
/// the shared Density plus the bias, unclamped (a gate at 45 biased +10 opens at a shared 35).
constexpr int32_t biasedGate(BiasModel model, int32_t shared, int32_t offset) {
	return model == BiasModel::CurvedGateShift ? shared + offset : biased(model, shared, offset);
}

/// One part's bias, the same for every step. part -1: no part biased, the Generator plays as without the prototype.
struct PartBias {
	int8_t part = -1;
	BiasModel model = BiasModel::Linear;
	int32_t units[kNumMacros] = {}; ///< Per Macro, in macro units, -kMacroFull..kMacroFull.
};

} // namespace deluge::generative::prototype
