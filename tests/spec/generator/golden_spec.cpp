// The goldens (spec §21.2, #121): each Techno subgenre plays its committed golden, its default Recipe at a fixed seed
// as a text dump plus its stream fingerprint. A failure prints where the dump parts ways with the file. A retune that
// means it follows spec §21.2 ("A retune"). See golden.h.

#include "golden.h"

#include "cppspec.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace harness;
using namespace harness::goldens;

namespace {

/// A two-part genre for the dump's format: a kit and a pitched part.
constexpr LaneSpec kKitLanes[] = {{.lane = Lane::Kick}, {.lane = Lane::ClosedHat}};
const PartSpec kParts[] = {
    {.role = Role::Drums, .name = "DRUMS", .lanes = kKitLanes},
    {.role = Role::Bass, .name = "BASS"},
};
const Genre kTwoParts = [] {
	Genre genre;
	genre.name = "two-parts";
	genre.parts = kParts;
	return genre;
}();

/// A file in the system's temp folder, removed when it goes out of scope.
struct TempFile {
	std::string path;
	explicit TempFile(const char* name) : path((std::filesystem::temp_directory_path() / name).string()) {
		std::filesystem::remove(path);
	}
	~TempFile() { std::filesystem::remove(path); }
	[[nodiscard]] std::string read() const {
		std::ifstream in(path, std::ios::binary);
		return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
	}
	void write(const std::string& text) const { std::ofstream(path, std::ios::binary) << text; }
};

/// Checks `genre`'s golden, or rewrites it when the run updates goldens; prints the diff when it differs.
bool checkGolden(const Genre& genre, const Golden& golden) {
	const Sync result = sync(pathOf(genre), golden.text, updating(), golden.lost);
	if (result.lossy) {
		std::printf("didn't update %s's golden %s: its play lost %u events to full buffers, so the dump isn't what "
		            "it plays. Fix the loss first\n",
		            genre.name, pathOf(genre).c_str(), golden.lost);
		return false;
	}
	if (result.wrote) {
		std::printf("updated %s's golden: %s\n", genre.name, pathOf(genre).c_str());
		return true;
	}
	if (result.unwritten) {
		std::printf("couldn't write %s's golden %s: is the folder there and writable?\n", genre.name,
		            pathOf(genre).c_str());
		return false;
	}
	if (!result.matched) {
		std::printf("%s plays other than its golden %s\n%s"
		            "If that's meant, raise the genre.version line in %s's table by one (spec 4.4) and run "
		            "./dbt test --generator --update-goldens\n",
		            genre.name, pathOf(genre).c_str(), result.diff.c_str(), genre.name);
	}
	return result.matched;
}

/// Each golden's spec title, kept here: CppSpec holds the pointer.
const std::vector<std::string> kGoldenTitles = [] {
	std::vector<std::string> titles;
	for (const Genre* genre : goldenGenres()) {
		titles.push_back(std::string(genre->name) + " plays its golden: its default Recipe for " + std::to_string(kBars)
		                 + " bars, and its fingerprint");
	}
	return titles;
}();

} // namespace

// clang-format off
describe golden("The goldens", $ {
	it("names notes with sharps, C4 = 60", _ {
		expect(harness::render::noteName(60)).to_equal(std::string("C4"));
		expect(harness::render::noteName(30)).to_equal(std::string("F#1"));
		expect(harness::render::noteName(0)).to_equal(std::string("C-1"));
		expect(harness::render::noteName(127)).to_equal(std::string("G9"));
	});

	it("places a tick by bar, beat and 16th from 1, with ticks past the 16th's line", _ {
		expect(positionOf(0)).to_equal(std::string("1.1.1"));
		expect(positionOf(kTicksPerStep * 7)).to_equal(std::string("1.2.4"));
		expect(positionOf(kBarTicks + kTicksPerStep * 9 + 12)).to_equal(std::string("2.3.2+12"));
	});

	it("dumps one event per line: tick, position, part, then what it says", _ {
		Event muted = Event::noteOn(48, 1, Lane::None, 43, 90);
		muted.muted = true;
		Event ornament = Event::noteOn(60, 0, Lane::ClosedHat, 0, 40);
		ornament.ornament = true;
		const std::vector<Event> events = {
		    Event::sectionStart(0, SectionKind::Groove, 1),
		    Event::keyScale(0, 7, Scale::Phrygian),
		    Event::controlChange(0, 1, Lane::None, SoundControl::Brightness, 12, false),
		    Event::controlChange(0, 0, Lane::Kick, SoundControl::Decay, -5, true),
		    Event::noteOn(0, 0, Lane::Kick, 0, 127),
		    Event::noteOn(24, 1, Lane::None, 43, 100),
		    Event::noteOff(36, 1, Lane::None, 43),
		    muted,
		    ornament,
		    Event::sound(kBarTicks, 1, 3),
		};
		expect(dumpEvents(kTwoParts, events)).to_equal(std::string(
		    "      0  1.1.1     SECTION  GROOVE 2\n"
		    "      0  1.1.1     KEY      G phrygian\n"
		    "      0  1.1.1     BASS     Brightness +12\n"
		    "      0  1.1.1     DRUMS    Kick Decay -5 snap\n"
		    "      0  1.1.1     DRUMS    Kick on v127\n"
		    "     24  1.1.2     BASS     G2 on v100\n"
		    "     36  1.1.2+12  BASS     G2 off\n"
		    "     48  1.1.3     BASS     G2 on v90 muted\n"
		    "     60  1.1.3+12  DRUMS    HatC on v40 ornament\n"
		    "    384  2.1.1     BASS     sound 3\n"));
	});

	it("shows where a dump parts ways with its golden: hunks of - golden and + fresh lines, one line of context", _ {
		expect(diffOf("a\nb\nc\n", "a\nb\nc\n")).to_equal(std::string());
		expect(diffOf("a\nb\nc\nd\n", "a\nx\nc\nd\n"))
		    .to_equal(std::string("@@ golden line 2, fresh line 2 @@\n 1: a\n-2: b\n+2: x\n 3: c\n"));
		expect(diffOf("a\nb\n", "a\nb\nc\n")).to_equal(std::string("@@ golden line 3, fresh line 3 @@\n 2: b\n+3: c\n"));
		expect(diffOf("a\nb\nc\n", "a\nc\n")).to_equal(std::string("@@ golden line 2, fresh line 2 @@\n 1: a\n-2: b\n 3: c\n"));
		expect(diffOf("a\nb\n", "a\nb")).to_equal(std::string("(only the file's last newline differs)\n"));
	});

	it("keeps far-apart changes in their own hunks, and only the changed lines, not all between", _ {
		expect(diffOf("a\nb\nc\nd\ne\nf\ng\nh\n", "a\nB\nc\nd\ne\nf\nG\nh\n"))
		    .to_equal(std::string("@@ golden line 2, fresh line 2 @@\n 1: a\n-2: b\n+2: B\n 3: c\n"
		                          "@@ golden line 7, fresh line 7 @@\n 6: f\n-7: g\n+7: G\n 8: h\n"));
		expect(diffOf("a\nb\nc\nd\n", "a\nB\nc\nD\n"))
		    .to_equal(std::string("@@ golden line 2, fresh line 2 @@\n 1: a\n-2: b\n+2: B\n 3: c\n-4: d\n+4: D\n"));
	});

	it("stops after `limit` changed lines and says how many more", _ {
		expect(diffOf("a\nb\nc\nd\ne\n", "a\nB\nC\nD\ne\n", 4))
		    .to_equal(std::string("@@ golden line 2, fresh line 2 @@\n 1: a\n-2: b\n-3: c\n-4: d\n+2: B\n"
		                          "... 2 more changed lines\n"));
	});

	it("rewrites a golden only when updating, and leaves a matching one be", _ {
		const TempFile file("deluge_golden_spec.txt");
		Sync missing = sync(file.path, "one\n", false);
		expect(missing.matched).to_be_false();
		expect(missing.wrote).to_be_false();
		expect(std::filesystem::exists(file.path)).to_be_false();

		Sync created = sync(file.path, "one\n", true);
		expect(created.wrote).to_be_true();
		expect(file.read()).to_equal(std::string("one\n"));

		Sync same = sync(file.path, "one\n", true);
		expect(same.matched).to_be_true();
		expect(same.wrote).to_be_false();

		Sync changed = sync(file.path, "two\n", false);
		expect(changed.matched).to_be_false();
		expect(changed.diff).to_equal(std::string("@@ golden line 1, fresh line 1 @@\n-1: one\n+1: two\n"));
		expect(file.read()).to_equal(std::string("one\n"));

		expect(sync(file.path, "two\n", true).wrote).to_be_true();
		expect(file.read()).to_equal(std::string("two\n"));
	});

	it("never writes a dump that lost events, even when updating", _ {
		const TempFile file("deluge_golden_spec_lossy.txt");
		file.write("one\n");
		const Sync lossy = sync(file.path, "two\n", true, 3);
		expect(lossy.matched).to_be_false();
		expect(lossy.wrote).to_be_false();
		expect(lossy.unwritten).to_be_false();
		expect(lossy.lossy).to_be_true();
		expect(file.read()).to_equal(std::string("one\n"));
		expect(sync(file.path, "two\n", false, 3).lossy).to_be_false();
		expect(sync(file.path, "one\n", true, 3).matched).to_be_true();
	});

	it("says when an update can't write the golden", _ {
		const std::string path =
		    (std::filesystem::temp_directory_path() / "deluge_golden_spec_no_such_dir" / "golden.txt").string();
		std::filesystem::remove_all(std::filesystem::path(path).parent_path());
		const Sync failed = sync(path, "one\n", true);
		expect(failed.wrote).to_be_false();
		expect(failed.unwritten).to_be_true();
		expect(sync(path, "one\n", false).unwritten).to_be_false();
	});

	it("writes a golden's header: the genre and version, the Recipe, the parts and the fingerprint", _ {
		Genre genre = kTwoParts;
		genre.version = 3;
		Recipe recipe;
		recipe.tempo = 13005;
		recipe.keyRoot = 7;
		recipe.scale = Scale::Phrygian;
		recipe.swing = -3;
		for (int32_t m = 0; m < kNumMacros; m++) {
			recipe.macros.set(static_cast<Macro>(m), macroPoints(10 * (m + 1)));
		}
		StreamFingerprint fingerprint;
		fingerprint.hash = 0x0123456789abcdefULL;
		fingerprint.events = 1234;
		const std::vector<std::string> lines = linesOf(goldenText(genre, recipe, {}, fingerprint));
		expect(lines.size()).to_equal(size_t{8});
		expect(lines[2]).to_equal(std::string("format ") + std::to_string(kFormat));
		expect(lines[3]).to_equal(std::string("genre two-parts version 3"));
		expect(lines[4]).to_equal("recipe entropy " + std::to_string(kEntropy)
		                          + ": G phrygian, 130.05 BPM, swing -3, energy 10, density 20, complexity 30, motion 40");
		expect(lines[5]).to_equal(std::string("parts 0 DRUMS, 1 BASS"));
		expect(lines[6]).to_equal("fingerprint 0123456789abcdef, 1234 events, 0 lost (" + std::to_string(kFingerprintPieces)
		                          + " pieces with random macros, scales, keys and seeds)");
		expect(lines[7]).to_equal(std::string());
	});

	it("fails a subgenre whose stream changed, and shows where", _ {
		// Acid at another default Density: its recipe line and its events change. Computed, not pinned, so a retune
		// of Acid's table goes through the update alone.
		Genre changed = genres::technoAcid;
		const int32_t density = macroToPoints(changed.defaults[Macro::Density]);
		changed.defaults.set(Macro::Density, macroPoints(density < 25 ? density + 25 : density - 25));
		const std::string golden = playGolden(genres::technoAcid, 0).text;
		const std::string fresh = playGolden(changed, 0).text;
		const TempFile file("deluge_golden_spec_acid.txt");
		file.write(golden);
		const Sync result = sync(file.path, fresh, false);
		expect(result.matched).to_be_false();
		expect(result.wrote).to_be_false();
		const std::vector<std::string> before = linesOf(golden);
		const std::vector<std::string> after = linesOf(fresh);
		const auto recipeAt = static_cast<size_t>(std::find_if(before.begin(), before.end(), [](const std::string& line) {
			                                          return line.starts_with("recipe ");
		                                          }) - before.begin());
		const std::string number = std::to_string(recipeAt + 1);
		expect(result.diff.find("-" + number + ": " + before[recipeAt] + "\n+" + number + ": " + after[recipeAt] + "\n")
		       != std::string::npos)
		    .to_be_true();
		// And a changed event: a line past the header's blank line.
		const auto header = static_cast<size_t>(std::find(before.begin(), before.end(), std::string()) - before.begin());
		bool event = false;
		for (const std::string& line : linesOf(result.diff)) {
			event = event || ((line.starts_with("-") || line.starts_with("+")) && std::stoul(line.substr(1)) > header + 1);
		}
		expect(event).to_be_true();
	});

	const std::vector<const Genre*> genres = goldenGenres();
	for (size_t g = 0; g < genres.size(); g++) {
		const Genre* genre = genres[g];
		it(kGoldenTitles[g].c_str(), _ {
			const Golden golden = playGolden(*genre);
			expect(golden.lost).to_equal(0U);
			expect(checkGolden(*genre, golden)).to_be_true();
		});
	}

	it("has one golden per Techno subgenre, and none for anything else (dotfiles aside)", _ {
		// After the goldens' own specs, so an update that adds a subgenre has written its golden by now.
		const std::vector<const Genre*> genres = goldenGenres();
		expect(genres.empty()).to_be_false();
		std::vector<std::string> expected;
		for (const Genre* genre : genres) {
			expected.push_back(std::string(genre->name) + ".txt");
		}
		std::vector<std::string> found;
		const std::filesystem::path folder = std::filesystem::path(pathOf(*genres[0])).parent_path();
		for (const auto& entry : std::filesystem::directory_iterator(folder)) {
			const std::string name = entry.path().filename().string();
			if (!name.starts_with(".")) { // .DS_Store and the like
				found.push_back(name);
			}
		}
		std::sort(expected.begin(), expected.end());
		std::sort(found.begin(), found.end());
		expect(found).to_equal(expected);
	});
});

CPPSPEC_SPEC(golden)
