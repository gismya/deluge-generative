// Rough render (spec §21.2, §21.5 step 1): one local command renders a Recipe and seed to a WAV, a MIDI file and a
// piano roll, and the same input renders the same bytes.

#include "render/command.h"

#include "cppspec.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace deluge::generative;
namespace fs = std::filesystem;

namespace {

harness::render::Parsed parse(std::vector<const char*> args) {
	args.insert(args.begin(), "generator_render");
	return harness::render::parseArgs(static_cast<int>(args.size()), args.data());
}

std::string slurp(const fs::path& path) {
	std::ifstream in(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

} // namespace

// clang-format off
describe render_command("Rough render command", $ {
	it("takes genre, Recipe overrides, seed and length", _ {
		harness::render::Parsed parsed = parse({"--genre", "techno", "--seed", "7", "--entropy", "3", "--bars", "8",
		                                        "--energy", "40", "--density", "0", "--complexity", "50",
		                                        "--motion", "12", "--key", "C#", "--scale", "phrygian",
		                                        "--tempo", "131.5", "--swing", "-10", "--out", "/tmp/x"});
		expect(parsed.error).to_equal(std::string{});
		const harness::render::Request& r = parsed.request;
		expect(std::string(r.overrides.genre)).to_equal(std::string("techno"));
		expect(r.overrides.seed).to_equal(7U);
		expect(r.overrides.entropy).to_equal(3U);
		expect(r.bars).to_equal(8);
		expect(r.overrides.macroPoints[0]).to_equal(int8_t{40});
		expect(r.overrides.macroPoints[1]).to_equal(int8_t{0});
		expect(r.overrides.macroPoints[2]).to_equal(int8_t{50});
		expect(r.overrides.macroPoints[3]).to_equal(int8_t{12});
		expect(r.overrides.keyRoot).to_equal(int8_t{1});
		expect(r.overrides.scale).to_equal(static_cast<int8_t>(Scale::Phrygian));
		expect(r.overrides.tempo).to_equal(13150);
		expect(r.overrides.swing).to_equal(int8_t{-10});
		expect(r.out).to_equal(std::string("/tmp/x"));
	});

	it("refuses what it can't play, saying why", _ {
		expect(parse({"--out", "x", "--genre", "polka"}).error).to_equal(std::string("unknown genre: polka"));
		expect(parse({"--out", "x", "--key", "H"}).error).to_equal(std::string("unknown key: H"));
		expect(parse({"--out", "x", "--scale", "blues"}).error).to_equal(std::string("unknown scale: blues"));
		expect(parse({"--out", "x", "--energy", "51"}).error).to_equal(std::string("--energy takes 0 to 50"));
		expect(parse({"--out", "x", "--loud"}).error).to_equal(std::string("unknown argument: --loud"));
		expect(parse({"--seed", "1"}).error).to_equal(std::string("--out is required"));
	});

	it("renders a WAV, a MIDI file and a piano roll, byte-identical twice in a row", _ {
		fs::path dir = fs::temp_directory_path() / "generator_render_spec" / "command";
		fs::remove_all(dir);
		fs::create_directories(dir);
		harness::render::Parsed parsed = parse({"--seed", "42", "--bars", "4", "--out", "unused"});
		harness::render::Request request = parsed.request;
		request.library.roots = {};

		request.out = (dir / "a").string();
		harness::render::render(request);
		request.out = (dir / "b").string();
		harness::render::render(request);

		for (const char* ext : {".wav", ".mid", ".txt"}) {
			std::string a = slurp(dir / (std::string("a") + ext));
			expect(a.size()).to_be_greater_than(size_t{100});
			expect(a == slurp(dir / (std::string("b") + ext))).to_be_true();
		}
		expect(slurp(dir / "a.txt").starts_with("techno seed 42,")).to_be_true();
	});
});

CPPSPEC_SPEC(render_command)
