// Rough render (spec §21.2): a WAV from our own small voices, one subtractive voice plus a sample player for kit
// rows, reading samples from pack/ and a local card-shaped folder.

#include "harness.h"
#include "render/render.h"

#include "cppspec.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace deluge::generative;
namespace fs = std::filesystem;

namespace {

harness::render::Rendering silentBar() {
	harness::render::Rendering rendering;
	rendering.recipe = harness::technoRecipe();
	rendering.recipe.tempo = 12800;
	rendering.sounds.part[1].set("SYNTHS/165 Acid Arp.XML");
	rendering.sounds.lane[static_cast<int32_t>(Lane::Kick)].set("SAMPLES/DRUMS/Kick/909 Kick.wav");
	rendering.bars = 1;
	return rendering;
}

uint32_t le32(const std::vector<uint8_t>& b, size_t at) {
	return b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | (static_cast<uint32_t>(b[at + 3]) << 24);
}
uint16_t le16(const std::vector<uint8_t>& b, size_t at) {
	return static_cast<uint16_t>(b[at] | (b[at + 1] << 8));
}

/// The left channel of a rendered 16-bit stereo WAV with a 44-byte header.
std::vector<int16_t> left(const std::vector<uint8_t>& wav) {
	std::vector<int16_t> out;
	for (size_t at = 44; at + 3 < wav.size(); at += 4) {
		out.push_back(static_cast<int16_t>(le16(wav, at)));
	}
	return out;
}

bool silent(const std::vector<int16_t>& s, size_t from, size_t to) {
	for (size_t i = from; i < to && i < s.size(); i++) {
		if (s[i] != 0) {
			return false;
		}
	}
	return true;
}

/// A mono 16-bit PCM WAV of `frames` frames at a constant level.
void writeSample(const fs::path& path, int32_t frames, int16_t level) {
	fs::create_directories(path.parent_path());
	std::vector<uint8_t> b;
	auto put32 = [&](uint32_t v) {
		for (int32_t i = 0; i < 4; i++) {
			b.push_back(static_cast<uint8_t>(v >> (8 * i)));
		}
	};
	auto put16 = [&](uint16_t v) {
		b.push_back(static_cast<uint8_t>(v));
		b.push_back(static_cast<uint8_t>(v >> 8));
	};
	b.insert(b.end(), {'R', 'I', 'F', 'F'});
	put32(36 + frames * 2);
	b.insert(b.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
	put32(16);
	put16(1);
	put16(1);
	put32(44100);
	put32(44100 * 2);
	put16(2);
	put16(16);
	b.insert(b.end(), {'d', 'a', 't', 'a'});
	put32(frames * 2);
	for (int32_t i = 0; i < frames; i++) {
		put16(static_cast<uint16_t>(level));
	}
	std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), b.size());
}

fs::path scratch(const char* name) {
	fs::path dir = fs::temp_directory_path() / "generator_render_spec" / name;
	fs::remove_all(dir);
	fs::create_directories(dir);
	return dir;
}

bool mentions(const std::vector<std::string>& warnings, const char* what) {
	for (const std::string& w : warnings) {
		if (w.find(what) != std::string::npos) {
			return true;
		}
	}
	return false;
}

} // namespace

// clang-format off
describe render_audio("Rough render audio", $ {
	it("writes 16-bit stereo at 44.1 kHz for the bars plus a second of tail, silent with no notes", _ {
		harness::render::Audio audio = harness::render::renderAudio(silentBar(), {});
		const std::vector<uint8_t>& wav = audio.wav;
		// One bar at 128 BPM is 82687 frames, and the tail 44100 more.
		const uint32_t frames = 82687 + 44100;
		expect(std::memcmp(wav.data(), "RIFF", 4)).to_equal(0);
		expect(std::memcmp(wav.data() + 8, "WAVEfmt ", 8)).to_equal(0);
		expect(le16(wav, 20)).to_equal(uint16_t{1});
		expect(le16(wav, 22)).to_equal(uint16_t{2});
		expect(le32(wav, 24)).to_equal(44100U);
		expect(le16(wav, 34)).to_equal(uint16_t{16});
		expect(std::memcmp(wav.data() + 36, "data", 4)).to_equal(0);
		expect(le32(wav, 40)).to_equal(frames * 4);
		expect(wav.size()).to_equal(size_t{44 + frames * 4});
		expect(silent(left(wav), 0, frames)).to_be_true();
	});

	it("leaves out muted notes, as Perform drops them at the output", _ {
		harness::render::Rendering rendering = silentBar();
		Event kick = Event::noteOn(0, 0, Lane::Kick, 0, 127);
		Event bass = Event::noteOn(96, 1, Lane::None, 45, 100);
		kick.muted = bass.muted = true;
		rendering.events = {kick, bass, Event::noteOff(144, 1, Lane::None, 45)};
		harness::render::Audio audio = harness::render::renderAudio(rendering, {});
		expect(silent(left(audio.wav), 0, 82687 + 44100)).to_be_true();
		expect(audio.warnings.empty()).to_be_true();
	});

	it("sounds a pitched note from its tick on, and warns once that the preset's parameters aren't modelled", _ {
		harness::render::Rendering rendering = silentBar();
		rendering.events = {
		    Event::noteOn(96, 1, Lane::None, 45, 100),
		    Event::noteOff(144, 1, Lane::None, 45),
		    Event::noteOn(192, 1, Lane::None, 48, 100),
		    Event::noteOff(240, 1, Lane::None, 48),
		};
		harness::render::Audio audio = harness::render::renderAudio(rendering, {});
		std::vector<int16_t> l = left(audio.wav);
		// Tick 96 is frame 20671.
		expect(silent(l, 0, 20671)).to_be_true();
		expect(silent(l, 20671, 20671 + 2000)).to_be_false();
		int32_t warnings = 0;
		for (const std::string& w : audio.warnings) {
			warnings += w.find("165 Acid Arp") != std::string::npos ? 1 : 0;
		}
		expect(warnings).to_equal(1);
	});

	it("plays a kit row's sample from the card folder, and a stand-in with a warning when it isn't there", _ {
		fs::path card = scratch("card");
		writeSample(card / "SAMPLES/DRUMS/Kick/909 Kick.wav", 100, 16000);
		harness::render::Rendering rendering = silentBar();
		rendering.events = {Event::noteOn(0, 0, Lane::Kick, 0, 127)};
		harness::render::Audio withCard = harness::render::renderAudio(rendering, {{card.string()}});
		std::vector<int16_t> l = left(withCard.wav);
		expect(silent(l, 0, 100)).to_be_false();
		expect(silent(l, 101, 44100)).to_be_true();
		expect(mentions(withCard.warnings, "909 Kick")).to_be_false();

		harness::render::Audio without = harness::render::renderAudio(rendering, {{scratch("empty").string()}});
		expect(silent(left(without.wav), 0, 2000)).to_be_false();
		expect(mentions(without.warnings, "909 Kick")).to_be_true();
	});

	it("looks in pack/ before the card folder", _ {
		fs::path pack = scratch("pack");
		fs::path card = scratch("card2");
		writeSample(pack / "SAMPLES/DRUMS/Kick/909 Kick.wav", 50, 16000);
		writeSample(card / "SAMPLES/DRUMS/Kick/909 Kick.wav", 300, 16000);
		harness::render::Rendering rendering = silentBar();
		rendering.events = {Event::noteOn(0, 0, Lane::Kick, 0, 127)};
		std::vector<int16_t> l =
		    left(harness::render::renderAudio(rendering, {{pack.string(), card.string()}}).wav);
		expect(silent(l, 0, 50)).to_be_false();
		expect(silent(l, 51, 400)).to_be_true();
	});
});

CPPSPEC_SPEC(render_audio)
