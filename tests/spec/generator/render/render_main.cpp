// generator_render: the rough render's command line (spec §21.2). See kUsage, or run it with --help.

#include "render/command.h"

#include "generative/perform/riser_timing.h"

#include <cstdio>

int main(int argc, char** argv) {
	harness::render::Parsed parsed = harness::render::parseArgs(argc, argv);
	if (parsed.help) {
		std::fputs(harness::render::kUsage, stdout);
		return 0;
	}
	if (!parsed.error.empty()) {
		std::fprintf(stderr, "generator_render: %s\n\n%s", parsed.error.c_str(), harness::render::kUsage);
		return 2;
	}
	if (parsed.listPalettes) {
		std::fputs(harness::render::pitchedPalettePresets().c_str(), stdout);
		return 0;
	}
	if (parsed.listRisers) {
		std::fputs(harness::render::riserLanes().c_str(), stdout);
		return 0;
	}
	if (parsed.riserFallbackFor >= 0) {
		const auto* fallback = deluge::generative::perform::fallbackFor(static_cast<int32_t>(parsed.riserFallbackFor));
		std::printf("%s\n", fallback != nullptr ? fallback->path : "-");
		return 0;
	}
	if (parsed.listSounds) {
		bool rolledAll = true;
		std::fputs(harness::render::soundPicks(parsed.request, &rolledAll).c_str(), stdout);
		if (!rolledAll) {
			std::fputs("warning: a sound folder couldn't be listed: some parts and lanes have no sound pick\n", stderr);
		}
		return 0;
	}
	harness::render::Rendered rendered = harness::render::render(parsed.request);
	for (const std::string& warning : rendered.warnings) {
		std::fprintf(stderr, "warning: %s\n", warning.c_str());
	}
	if (!rendered.error.empty()) {
		std::fprintf(stderr, "generator_render: %s\n", rendered.error.c_str());
		return 1;
	}
	const std::string& out = parsed.request.out;
	if (parsed.request.midiOnly) {
		std::printf("%s.mid\n", out.c_str());
	}
	else {
		std::printf("%s.wav\n%s.mid\n%s.txt\n", out.c_str(), out.c_str(), out.c_str());
	}
	return 0;
}
