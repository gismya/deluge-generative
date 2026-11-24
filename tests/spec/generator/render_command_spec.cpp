// Rough render (spec §21.2, §21.5 step 1): one local command renders a Recipe and seed to a WAV, a MIDI file and a
// piano roll, and the same input renders the same bytes. For the theory review (§21.5) it can play one Arc cycle and
// write the MIDI file alone.

#include "render/command.h"
#include "render/names.h"
#include "render/render.h"
#include "temp_path.h"

#include "generative/core/genre.h"

#include "cppspec.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace deluge::generative;
namespace fs = std::filesystem;

namespace {

const harness::RemovedAtExit scratchRoot{harness::tempPath("generator_render_command_spec")};

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
		harness::render::Parsed parsed = parse({"--genre", "techno-acid", "--seed", "7", "--entropy", "3", "--bars", "8",
		                                        "--energy", "40", "--density", "0", "--complexity", "50",
		                                        "--motion", "12", "--key", "C#", "--scale", "phrygian",
		                                        "--tempo", "131.5", "--swing", "-10", "--out", "/tmp/x"});
		expect(parsed.error).to_equal(std::string{});
		const harness::render::Request& r = parsed.request;
		expect(std::string(r.overrides.genre)).to_equal(std::string("techno-acid"));
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

	it("takes --end-after-drop and --midi-only, which need no value", _ {
		harness::render::Parsed parsed = parse({"--end-after-drop", "--midi-only", "--bars", "8"});
		expect(parsed.error).to_equal(std::string{});
		expect(parsed.request.endAfterDrop).to_equal(1);
		expect(parsed.request.midiOnly).to_be_true();
		expect(parsed.request.bars).to_equal(8);
		expect(parse({}).request.endAfterDrop).to_equal(0);
		expect(parse({}).request.endAtBar).to_equal(-1);
		expect(parse({}).request.midiOnly).to_be_false();
	});

	it("takes a Drop count after --end-after-drop, and --end-at-bar, but not both", _ {
		expect(parse({"--end-after-drop", "2", "--bars", "8"}).request.endAfterDrop).to_equal(2);
		expect(parse({"--end-after-drop", "2", "--bars", "8"}).request.bars).to_equal(8);
		expect(parse({"--end-at-bar", "40"}).request.endAtBar).to_equal(40);
		expect(parse({"--end-after-drop", "0"}).error).to_equal(std::string("--end-after-drop takes a Drop count, 1 to 99"));
		expect(parse({"--end-after-drop", "x"}).error).to_equal(std::string("--end-after-drop takes a Drop count, 1 to 99"));
		expect(parse({"--end-at-bar", "-1"}).error).to_equal(std::string("--end-at-bar takes a bar, 0 to 9999"));
		expect(parse({"--end-after-drop", "--end-at-bar", "4"}).error)
		    .to_equal(std::string("--end-after-drop and --end-at-bar: give one"));
	});

	it("records the step that decided each Ornament hit, for the THEORY track", _ {
		harness::render::Request request = parse({"--genre", "techno-detroit", "--complexity", "50", "--bars", "48"}).request;
		harness::render::Rendering rendering = harness::render::playRequest(request);
		int32_t ornaments = 0;
		for (const Event& e : rendering.events) {
			if (e.kind != EventKind::NoteOn || !e.ornament) {
				continue;
			}
			ornaments++;
			const auto decided = std::find_if(rendering.ornamentSteps.begin(), rendering.ornamentSteps.end(),
			                                  [&](const auto& hit) { return hit.first == e; });
			expect(decided != rendering.ornamentSteps.end()).to_be_true();
			// A ratchet's or roll's later hits stay inside their step; a flam's grace leads it by half a 16th at most.
			const int32_t offset = e.tick - decided->second * kTicksPerStep;
			expect(offset >= -kMaxLean && offset < kTicksPerStep + kMaxLean).to_be_true();
		}
		expect(ornaments).to_be_greater_than(10);
	});

	it("with --end-after-drop plays one Arc cycle, the Drop's phrase then the Outro, and stops where the piece ends", _ {
		harness::render::Request request = parse({"--genre", "techno-acid", "--end-after-drop", "--bars", "1000"}).request;
		harness::render::Rendering rendering = harness::render::playRequest(request);
		std::vector<Event> sections;
		for (const Event& e : rendering.events) {
			if (e.kind == EventKind::Section) {
				sections.push_back(e);
			}
		}
		expect(sections.size()).to_be_greater_than(size_t{4});
		expect(sections.back().section == SectionKind::Outro).to_be_true();
		const Event& drop = sections[sections.size() - 2];
		expect(drop.section == SectionKind::Groove && drop.occurrence == 1).to_be_true();
		expect(sections[sections.size() - 3].section == SectionKind::Build).to_be_true();
		// The Drop plays a phrase (8 bars), the Outro one more, and the piece ends on its last bar line.
		const int32_t bar = kStepsPerBar * kTicksPerStep;
		expect(sections.back().tick - drop.tick).to_equal(8 * bar);
		expect(rendering.bars * bar - sections.back().tick).to_equal(8 * bar);
		expect(rendering.steps.size()).to_equal(static_cast<size_t>(rendering.bars) * kStepsPerBar);
	});

	it("with --end-at-bar at the Drop's bar plays what --end-after-drop plays; with a count, the Nth Drop's", _ {
		harness::render::Request once = parse({"--genre", "techno-acid", "--end-after-drop", "--bars", "1000"}).request;
		const harness::render::Rendering first = harness::render::playRequest(once);
		std::vector<Event> sections;
		for (const Event& e : first.events) {
			if (e.kind == EventKind::Section) {
				sections.push_back(e);
			}
		}
		const int32_t bar = kStepsPerBar * kTicksPerStep;
		const int32_t dropBar = sections[sections.size() - 2].tick / bar;
		harness::render::Request atBar = parse({"--genre", "techno-acid", "--bars", "1000"}).request;
		atBar.endAtBar = dropBar;
		bool armed = false;
		const harness::render::Rendering same = harness::render::playRequest(atBar, nullptr, &armed);
		expect(armed).to_be_true();
		expect(same.bars).to_equal(first.bars);
		expect(same.events == first.events).to_be_true();

		harness::render::Request twice = parse({"--genre", "techno-acid", "--end-after-drop", "2", "--bars", "1000"}).request;
		const harness::render::Rendering second = harness::render::playRequest(twice);
		std::vector<Event> later;
		int32_t drops = 0;
		for (const Event& e : second.events) {
			if (e.kind == EventKind::Section) {
				later.push_back(e);
				drops += e.section == SectionKind::Groove && later.size() > 1
				                 && later[later.size() - 2].section == SectionKind::Build
				             ? 1
				             : 0;
			}
		}
		expect(drops).to_equal(2);
		expect(later.back().section == SectionKind::Outro).to_be_true();
		expect(later[later.size() - 3].section == SectionKind::Build).to_be_true();
		expect(second.bars).to_be_greater_than(first.bars);
	});

	it("with --end-at-bar past --bars warns that End was never armed", _ {
		harness::render::Request request =
		    parse({"--genre", "techno-acid", "--end-at-bar", "20", "--bars", "8", "--midi-only", "--out", "unused"})
		        .request;
		request.out = (scratchRoot.path / "no-end-bar" / "a").string();
		const harness::render::Rendered rendered = harness::render::render(request);
		expect(std::find(rendered.warnings.begin(), rendered.warnings.end(),
		                 std::string("--end-at-bar: no bar 20 in 8 bars, so End was never armed and the piece has no "
		                             "Outro"))
		       != rendered.warnings.end())
		    .to_be_true();
	});

	it("with --end-after-drop warns when no Drop came in --bars, so the piece has no Outro, beside the WAV's warnings", _ {
		harness::render::Request request =
		    parse({"--genre", "techno-acid", "--end-after-drop", "--bars", "8", "--out", "unused"}).request;
		request.library.roots = {}; // every sample missing: the WAV warns too
		request.out = (scratchRoot.path / "no-drop" / "a").string();
		bool armed = true;
		harness::render::playRequest(request, nullptr, &armed);
		expect(armed).to_be_false();
		const harness::render::Rendered rendered = harness::render::render(request);
		expect(rendered.error).to_equal(std::string{});
		expect(std::find(rendered.warnings.begin(), rendered.warnings.end(),
		                 std::string("--end-after-drop: no Drop in 8 bars, so End was never armed and the piece has no "
		                             "Outro"))
		       != rendered.warnings.end())
		    .to_be_true();
	});

	it("writes only the MIDI file with --midi-only", _ {
		fs::path dir = scratchRoot.path / "midi-only";
		fs::remove_all(dir);
		fs::create_directories(dir);
		harness::render::Request request = parse({"--midi-only", "--bars", "2", "--out", "unused"}).request;
		request.out = (dir / "a").string();
		expect(harness::render::render(request).error).to_equal(std::string{});
		expect(fs::exists(dir / "a.mid")).to_be_true();
		expect(fs::exists(dir / "a.wav")).to_be_false();
		expect(fs::exists(dir / "a.txt")).to_be_false();
	});

	it("refuses what it can't play, saying why", _ {
		expect(parse({"--out", "x", "--genre", "polka"}).error).to_equal(std::string("unknown genre: polka"));
		expect(parse({"--out", "x", "--key", "H"}).error).to_equal(std::string("unknown key: H"));
		expect(parse({"--out", "x", "--scale", "blues"}).error).to_equal(std::string("unknown scale: blues"));
		expect(parse({"--out", "x", "--energy", "51"}).error).to_equal(std::string("--energy takes 0 to 50"));
		expect(parse({"--out", "x", "--loud"}).error).to_equal(std::string("unknown argument: --loud"));
		expect(parse({"--tempo", "nan"}).error).to_equal(std::string("--tempo takes 20 to 999 BPM"));
		expect(parse({"--tempo", "19.99"}).error).to_equal(std::string("--tempo takes 20 to 999 BPM"));
		expect(parse({"--seed", "4294967296"}).error).to_equal(std::string("--seed takes a 32-bit number"));
	});

	it("takes --variant SET=NAME, repeatable, by the genre's stable names, whatever order the flags come in", _ {
		const harness::render::Parsed parsed = parse({"--variant", "skank=plain", "--genre", "dub", "--out", "x"});
		expect(parsed.error).to_equal(std::string{});
		expect(parsed.request.overrides.variant[0]).to_equal(int8_t{1});
		expect(parsed.request.overrides.variant[1]).to_equal(int8_t{-1});
		const RolledRecipe rolled = rollWithOverrides(genres::dub, parsed.request.overrides);
		expect(rolled.recipe.variants.pick[0] == 1 && rolled.recipe.variants.allowed[0] == 0b10).to_be_true();
		// The THEORY track's recipe line names it, for the listening tools' check.
		const harness::render::Rendering rendering{rolled.recipe, {}, 1, 0, 1, {}, {}};
		expect(harness::render::theoryText(rendering)[0].second.find(" variant.skank=plain ") != std::string::npos).to_be_true();
		expect(parse({"--genre", "dub", "--variant", "skank=jazz", "--out", "x"}).error)
		    .to_equal(std::string("unknown variant for dub: skank=jazz (have skank=sevenths, skank=plain, drums=onedrop, drums=rockers, "
		                        "drums=steppers)"));
		expect(parse({"--variant", "skank=plain", "--out", "x"}).error)
		    .to_equal(std::string("unknown variant for techno-acid: skank=plain (have none)"));
	});

	it("reads numbers in base 10, as gen_emu.py does", _ {
		expect(parse({"--seed", "010"}).request.overrides.seed).to_equal(10U);
		expect(parse({"--entropy", "0x10"}).error).to_equal(std::string("--entropy takes a 32-bit number"));
		expect(parse({"--bars", "010"}).request.bars).to_equal(10);
	});

	it("refuses a render past an hour of audio before it plays, saying why", _ {
		harness::render::Request request = parse({"--bars", "301", "--tempo", "20", "--out", "unused"}).request;
		request.library.roots = {};
		expect(harness::render::render(request).error)
		    .to_equal(std::string("301 bars at 20 BPM make 1:00:13 of audio; the render writes an hour at most: ask for "
		                          "fewer bars"));
		expect(harness::render::audioLengthError(299, 2000)).to_equal(std::string{});
	});

	it("writes under the repo's build/renders/ when no --out is given", _ {
		const std::string out = parse({"--genre", "techno-acid", "--seed", "7"}).request.out;
		expect(out.ends_with("/build/renders/techno-acid-seed7")).to_be_true();
	});

	it("reports a file it can't write", _ {
		// A folder can't be made under a plain file, on any OS.
		fs::path dir = scratchRoot.path / "unwritable";
		fs::remove_all(dir);
		fs::create_directories(dir);
		std::ofstream(dir / "file") << "x";
		const std::string out = (dir / "file" / "x").string();
		harness::render::Request request = parse({"--bars", "1", "--out", out.c_str()}).request;
		request.library.roots = {};
		harness::render::Rendered rendered = harness::render::render(request);
		expect(rendered.error).to_equal("can't write " + out + ".wav");
	});

	it("renders a WAV, a MIDI file and a piano roll, byte-identical twice in a row", _ {
		fs::path dir = scratchRoot.path / "command";
		fs::remove_all(dir);
		fs::create_directories(dir);
		harness::render::Parsed parsed = parse({"--seed", "42", "--bars", "4", "--out", "unused"});
		harness::render::Request request = parsed.request;
		request.library.roots = {};

		request.out = (dir / "a").string();
		expect(harness::render::render(request).error).to_equal(std::string{});
		request.out = (dir / "b").string();
		expect(harness::render::render(request).error).to_equal(std::string{});

		for (const char* ext : {".wav", ".mid", ".txt"}) {
			std::string a = slurp(dir / (std::string("a") + ext));
			expect(a.size()).to_be_greater_than(size_t{100});
			expect(a == slurp(dir / (std::string("b") + ext))).to_be_true();
		}
		expect(slurp(dir / "a.txt").starts_with("techno-acid seed 42,")).to_be_true();
	});

	it("takes --mute PART, a sidebar row in the genre's part order, repeatable, as gen_emu.py capture does", _ {
		harness::render::Parsed parsed = parse({"--mute", "2", "--mute", "4", "--mute", "2"});
		expect(parsed.error).to_equal(std::string{});
		expect(parsed.request.mutedParts).to_equal(uint32_t{(1U << 1) | (1U << 3)});
		expect(parse({}).request.mutedParts).to_equal(uint32_t{0});
	});

	it("refuses a --mute past the genre's parts before it renders", _ {
		expect(parse({"--mute", "7"}).error).to_equal(std::string("--mute takes a part, 1 to 6"));
		expect(parse({"--mute", "0"}).error).to_equal(std::string("--mute takes a part, 1 to 6"));
		expect(parse({"--mute", "x"}).error).to_equal(std::string("--mute takes a part, 1 to 6"));
		// A genre with fewer parts than the sidebar has rows: its own count, whichever order the flags come in.
		const Genre* small = nullptr;
		for (const Genre* genre : allGenres()) {
			small = genre->parts.size() < kMaxParts ? genre : small;
		}
		expect(small != nullptr).to_be_true();
		if (small != nullptr) {
			const std::string past = std::to_string(small->parts.size() + 1);
			const std::string why = std::string(small->name) + " has " + std::to_string(small->parts.size())
			                        + " parts: --mute takes 1 to " + std::to_string(small->parts.size());
			expect(parse({"--mute", past.c_str(), "--genre", small->name}).error).to_equal(why);
		}
	});

	it("with --mute leaves the muted parts out of the WAV from step 0: silent wherever the one part left rests", _ {
		fs::path dir = scratchRoot.path / "mute-silent";
		fs::remove_all(dir);
		fs::create_directories(dir);
		const Genre& acid = *findGenre("techno-acid");
		constexpr int32_t kKept = 1; // BASS, sidebar row 2
		std::vector<std::string> rows;
		for (size_t p = 0; p < acid.parts.size(); p++) {
			if (static_cast<int32_t>(p) != kKept) {
				rows.push_back(std::to_string(p + 1));
			}
		}
		std::vector<const char*> args = {"--genre", "techno-acid", "--seed", "3", "--bars", "16", "--energy", "40", "--out", "unused"};
		for (const std::string& row : rows) {
			args.push_back("--mute");
			args.push_back(row.c_str());
		}
		harness::render::Request request = parse(args).request;
		request.library.roots = {};
		request.out = (dir / "a").string();
		expect(harness::render::render(request).error).to_equal(std::string{});
		const std::string wav = slurp(dir / "a.wav");

		// Where the kept part sounds: from each heard note-on to its note-off, plus the stand-in voice's release.
		const harness::render::Rendering rendering = harness::render::playRequest(request);
		const int64_t tempo = rendering.recipe.tempo;
		auto frameAt = [&](int32_t tick) { return int64_t{tick} * harness::render::kSampleRate * 6000 / (tempo * 96); };
		constexpr int64_t kRelease = 6000; // the voice falls to its cut-off in about 5,100 frames
		const int64_t frames = static_cast<int64_t>(wav.size() - 44) / 4;
		std::vector<bool> sounding(static_cast<size_t>(frames), false);
		std::vector<int64_t> on(128, -1);
		int32_t notes = 0;
		for (const Event& e : rendering.events) {
			if (e.part != kKept || !harness::render::heard(e)) {
				continue;
			}
			if (e.kind == EventKind::NoteOn) {
				on[e.pitch] = frameAt(e.tick);
				notes++;
			}
			else if (e.kind == EventKind::NoteOff && on[e.pitch] >= 0) {
				for (int64_t f = on[e.pitch]; f < std::min(frames, frameAt(e.tick) + kRelease); f++) {
					sounding[static_cast<size_t>(f)] = true;
				}
				on[e.pitch] = -1;
			}
		}
		for (int64_t start : on) {
			for (int64_t f = start; start >= 0 && f < frames; f++) {
				sounding[static_cast<size_t>(f)] = true;
			}
		}
		expect(notes).to_be_greater_than(8);
		int64_t loudWhileSounding = 0;
		int64_t loudAtRest = 0;
		int64_t rest = 0;
		for (int64_t f = 0; f < frames; f++) {
			const size_t at = 44 + static_cast<size_t>(f) * 4;
			const bool loud = wav[at] != 0 || wav[at + 1] != 0;
			rest += sounding[static_cast<size_t>(f)] ? 0 : 1;
			(sounding[static_cast<size_t>(f)] ? loudWhileSounding : loudAtRest) += loud ? 1 : 0;
		}
		expect(rest).to_be_greater_than(harness::render::kSampleRate); // the bass rests, so the check checks
		expect(loudAtRest).to_equal(int64_t{0});
		expect(loudWhileSounding).to_be_greater_than(int64_t{0});
	});

	it("with --mute keeps every part in the MIDI file and the piano roll, byte for byte", _ {
		fs::path dir = scratchRoot.path / "mute-midi";
		fs::remove_all(dir);
		fs::create_directories(dir);
		harness::render::Request plain = parse({"--seed", "5", "--bars", "4", "--out", "unused"}).request;
		harness::render::Request muted =
		    parse({"--seed", "5", "--bars", "4", "--mute", "1", "--mute", "2", "--out", "unused"}).request;
		plain.library.roots = muted.library.roots = {};
		plain.out = (dir / "plain").string();
		muted.out = (dir / "muted").string();
		expect(harness::render::render(plain).error).to_equal(std::string{});
		expect(harness::render::render(muted).error).to_equal(std::string{});
		expect(slurp(dir / "plain.mid") == slurp(dir / "muted.mid")).to_be_true();
		expect(slurp(dir / "plain.txt") == slurp(dir / "muted.txt")).to_be_true();
		expect(slurp(dir / "plain.wav") == slurp(dir / "muted.wav")).to_be_false();
	});

	it("lists each built-in genre's pitched parts' palette presets with --list-palettes, for osc_octaves.py", _ {
		harness::render::Parsed parsed = parse({"--list-palettes"});
		expect(parsed.error).to_equal(std::string{});
		expect(parsed.listPalettes).to_be_true();
		expect(parse({"--list-palettes", "--seed", "1"}).error.empty()).to_be_false(); // a mode of its own

		const std::string listing = harness::render::pitchedPalettePresets();
		expect(listing.find("techno-dub\tSUB\tSYNTHS/002 Basic Square Bass.XML\n")).not_().to_equal(std::string::npos);
		expect(listing.find("techno-acid\tDRONE\tSYNTHS/148 Warm 5th Pad.XML\n")).not_().to_equal(std::string::npos);
		// Not a Kit's lanes: the FX lanes' synths keep their transpose, and Drums play samples
		expect(listing.find("GEN Impact")).to_equal(std::string::npos);
		expect(listing.find("\tDRUMS\t")).to_equal(std::string::npos);
		expect(listing.find("\tFX\t")).to_equal(std::string::npos);
	});

	it("lists each built-in genre's Riser lane with its shortest Build with --list-risers, for riser_fit.py", _ {
		harness::render::Parsed parsed = parse({"--list-risers"});
		expect(parsed.error).to_equal(std::string{});
		expect(parsed.listRisers).to_be_true();
		expect(parse({"--list-risers", "--seed", "1"}).error.empty()).to_be_false(); // a mode of its own

		const std::string listing = harness::render::riserLanes();
		// Dub's 1-bar Build at 80 BPM is 3,000 ms; its lane reads the short riser's own folder.
		expect(listing.find("dub\t3000\tSAMPLES/FX/Riser/Short\t\n")).not_().to_equal(std::string::npos);
		// Acid techno's 8-bar Build at its top tempo, every file in the default folder.
		expect(listing.find("techno-acid\t")).not_().to_equal(std::string::npos);
		expect(listing.find("psytrance-classic\t13150\tSAMPLES/FX/Riser\t\n")).not_().to_equal(std::string::npos);
	});

	it("lists a Recipe's sound picks with --list-sounds: a line per part and per lane of a Drums or FX part", _ {
		harness::render::Parsed parsed = parse({"--list-sounds", "--genre", "dub", "--entropy", "12", "--seed", "3"});
		expect(parsed.error).to_equal(std::string{});
		expect(parsed.listSounds).to_be_true();
		harness::render::Request request = parsed.request;
		request.library.roots = {GENERATOR_REPO_DIR "/pack"};

		bool rolledAll = false;
		const std::string listing = harness::render::soundPicks(request, &rolledAll);
		expect(rolledAll).to_be_true();
		const Genre& dub = *findGenre("dub");
		for (const PartSpec& part : dub.parts) {
			if (part.lanes.empty()) {
				expect(listing.find(std::string(part.name) + "\t")).not_().to_equal(std::string::npos);
			}
			for (const LaneSpec& lane : part.lanes) {
				const std::string row = std::string(part.name) + "."
				                        + harness::render::kLaneNames[static_cast<int32_t>(lane.lane)] + "\t";
				expect(listing.find(row)).not_().to_equal(std::string::npos);
			}
		}
		// The pack's own short riser, as the render's WAV plays it; no kit on pack/ alone, so no drum sample
		expect(listing.find("FX.Riser\tSAMPLES/FX/Riser/Short/")).not_().to_equal(std::string::npos);
		expect(listing.find("DRUMS.Kick\t-\n")).not_().to_equal(std::string::npos);
		expect(harness::render::soundPicks(request, &rolledAll)).to_equal(listing); // the same Recipe, the same picks

		// The kit's machine, from the entropy alone: named though pack/ holds no drum sample
		const size_t kit = listing.find("DRUMS.kit\t");
		expect(kit).not_().to_equal(std::string::npos);
		const std::string machine = listing.substr(kit + 10, listing.find('\t', kit + 10) - kit - 10);
		bool known = false;
		for (const MachineWeight& each : dub.palette->kits.machines) {
			known = known || machine == each.machine->name;
		}
		expect(known).to_be_true();

		// Another entropy rolls other sounds
		request.overrides.entropy = 13;
		expect(harness::render::soundPicks(request, &rolledAll) == listing).to_be_false();
	});
});

CPPSPEC_SPEC(render_command)
