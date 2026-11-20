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
};

/// `shared`, the value every part plays at after the Arc (effectiveOn()), moved by the bias `offset`, both in macro
/// units. Within 0..kMacroFull.
constexpr int32_t biased(BiasModel model, int32_t shared, int32_t offset) {
	if (model == BiasModel::Linear) {
		return std::clamp<int32_t>(shared + offset, 0, kMacroFull);
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
