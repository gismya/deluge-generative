// generator_render: the rough render's command line (spec §21.2). See kUsage, or run it with --help.

#include "render/command.h"

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
	harness::render::Rendered rendered = harness::render::render(parsed.request);
	for (const std::string& warning : rendered.warnings) {
		std::fprintf(stderr, "warning: %s\n", warning.c_str());
	}
	if (!rendered.error.empty()) {
		std::fprintf(stderr, "generator_render: %s\n", rendered.error.c_str());
		return 1;
	}
	const std::string& out = parsed.request.out;
	std::printf("%s.wav\n%s.mid\n%s.txt\n", out.c_str(), out.c_str(), out.c_str());
	return 0;
}
