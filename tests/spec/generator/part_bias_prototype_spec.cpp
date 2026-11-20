// PROTOTYPE (#414, throwaway): Part bias's curve candidates and their harness-only hook. #303 builds the real thing;
// this spec checks only what the prototype's renders rely on.

#include "harness.h"

#include "generative/core/part_bias_prototype.h"
#include "generative/core/recipe_overrides.h"

#include "cppspec.hpp"

#include <vector>

using namespace harness;
using namespace deluge::generative;
using prototype::BiasModel;

namespace {

int32_t noteOns(const std::vector<Event>& events, int32_t part) {
	int32_t n = 0;
	for (const Event& e : events) {
		n += e.kind == EventKind::NoteOn && e.part == part ? 1 : 0;
	}
	return n;
}

Result playBiased(int32_t density, int32_t part, BiasModel model, Macro macro, int32_t points) {
	RecipeOverrides overrides;
	overrides.genre = "psytrance";
	overrides.entropy = 1;
	overrides.macroPoints[static_cast<int32_t>(Macro::Density)] = static_cast<int8_t>(density);
	const RolledRecipe rolled = rollWithOverrides(*allGenres()[0], overrides);
	Run run;
	run.recipe = rolled.recipe;
	run.seed = rolled.seed;
	run.skipIntro = true;
	prototype::PartBias bias;
	bias.part = static_cast<int8_t>(part);
	bias.model = model;
	bias.units[static_cast<int32_t>(macro)] = macroPoints(points);
	Generator::testBias_ = bias;
	Result result = play(run, 16);
	Generator::testBias_ = {};
	return result;
}

} // namespace

// clang-format off
describe part_bias_prototype("Part bias prototype", $ {
	it("is the identity at bias 0 for every model", _ {
		for (BiasModel model : {BiasModel::Linear, BiasModel::Curved, BiasModel::CurvedGateShift}) {
			for (int32_t v = 0; v <= kMacroFull; v += 25) {
				expect(prototype::biased(model, v, 0)).to_equal(v);
				expect(prototype::biasedGate(model, v, 0)).to_equal(v);
			}
		}
	});

	it("keeps both ends reachable and stays within 0..50 for the curved model", _ {
		for (int32_t o = -kMacroFull; o <= kMacroFull; o += 64) {
			expect(prototype::biased(BiasModel::Curved, 0, o)).to_be_between(int32_t{0}, kMacroFull);
			expect(prototype::biased(BiasModel::Curved, kMacroFull, o)).to_be_between(int32_t{0}, kMacroFull);
		}
		expect(prototype::biased(BiasModel::Curved, kMacroFull, -macroPoints(20))).to_be_less_than(kMacroFull);
		expect(prototype::biased(BiasModel::Curved, kMacroFull, macroPoints(20))).to_equal(kMacroFull);
		expect(prototype::biased(BiasModel::Curved, 0, -macroPoints(20))).to_equal(0);
		expect(prototype::biased(BiasModel::Curved, 0, macroPoints(20))).to_be_greater_than(0);
		expect(prototype::biased(BiasModel::Curved, macroPoints(25), macroPoints(10))).to_equal(macroPoints(30));
		expect(prototype::biased(BiasModel::Curved, macroPoints(25), -macroPoints(10))).to_equal(macroPoints(20));
	});

	it("clamps the linear model at 0 and 50", _ {
		expect(prototype::biased(BiasModel::Linear, macroPoints(45), macroPoints(10))).to_equal(kMacroFull);
		expect(prototype::biased(BiasModel::Linear, macroPoints(5), -macroPoints(10))).to_equal(0);
		expect(prototype::biased(BiasModel::Linear, macroPoints(20), macroPoints(8))).to_equal(macroPoints(28));
	});

	it("is monotonic in the shared value and in the bias for every model", _ {
		for (BiasModel model : {BiasModel::Linear, BiasModel::Curved, BiasModel::CurvedGateShift}) {
			for (int32_t o = -kMacroFull; o <= kMacroFull; o += 160) {
				for (int32_t v = 0; v < kMacroFull; v += 5) {
					const int32_t here = prototype::biased(model, v, o);
					expect(prototype::biased(model, v + 5, o) >= here).to_be_true();
					expect(prototype::biased(model, v, o + 160) >= here).to_be_true();
				}
			}
		}
	});

	it("moves a Density gate as a threshold shift in the gate-shift model, unclamped", _ {
		expect(prototype::biasedGate(BiasModel::CurvedGateShift, macroPoints(45), macroPoints(10))).to_equal(macroPoints(55));
		expect(prototype::biasedGate(BiasModel::Curved, macroPoints(45), macroPoints(10)))
		    .to_equal(prototype::biased(BiasModel::Curved, macroPoints(45), macroPoints(10)));
	});

	it("leaves the event stream unchanged when the bias is 0", _ {
		Generator::testBias_ = {};
		RecipeOverrides overrides;
		overrides.genre = "psytrance";
		overrides.entropy = 1;
		overrides.macroPoints[static_cast<int32_t>(Macro::Density)] = 35;
		const RolledRecipe rolled = rollWithOverrides(*allGenres()[0], overrides);
		Run run;
		run.recipe = rolled.recipe;
		run.seed = rolled.seed;
		run.skipIntro = true;
		const Result plain = play(run, 16);
		for (BiasModel model : {BiasModel::Linear, BiasModel::Curved, BiasModel::CurvedGateShift}) {
			const Result zero = playBiased(35, 3, model, Macro::Density, 0);
			expect(zero.events == plain.events).to_be_true();
		}
	});

	it("opens a part's Density gate with a bias up, and closes it with a bias down", _ {
		const int32_t lead = 3; // psytrance's LEAD, gated at Density 30
		expect(noteOns(playBiased(25, lead, BiasModel::Curved, Macro::Density, 0).events, lead)).to_equal(0);
		expect(noteOns(playBiased(25, lead, BiasModel::Curved, Macro::Density, 20).events, lead)).to_be_greater_than(0);
		const int32_t open = noteOns(playBiased(35, lead, BiasModel::Curved, Macro::Density, 0).events, lead);
		const int32_t down = noteOns(playBiased(35, lead, BiasModel::Curved, Macro::Density, -20).events, lead);
		expect(open).to_be_greater_than(0);
		expect(down).to_be_less_than(open);
	});
});

CPPSPEC_SPEC(part_bias_prototype);
