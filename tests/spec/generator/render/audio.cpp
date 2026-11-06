// Rough render: the audio. Our own small voices, not the firmware's DSP (spec §21.2): a sample player for kit rows,
// a stand-in hit for a kit row with no sample, and one subtractive voice (saw, resonant low-pass, envelopes) for every
// pitched part. Plain single-threaded float math with no random source but a fixed LCG, so renders repeat exactly.

#include "render/render.h"

#include "generative/core/genre.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <numbers>
#include <optional>

namespace harness::render {

namespace {

namespace fs = std::filesystem;

constexpr float kTwoPi = 2.f * std::numbers::pi_v<float>;
constexpr float kSampleGain = 0.5f;
constexpr float kSynthGain = 0.2f;

int64_t frameAt(int32_t tick, int32_t tempo) {
	// tempo is in hundredths of a BPM: 96 ticks per beat, so tick * 60 * 100 / (tempo * 96) seconds.
	return static_cast<int64_t>(std::max(tick, 0)) * kSampleRate * 6000 / (static_cast<int64_t>(tempo) * 96);
}

// ---- Samples ----

struct Sample {
	std::vector<float> frames; ///< Mono.
	int32_t rate = kSampleRate;
};

uint32_t le(const std::vector<uint8_t>& b, size_t at, int32_t bytes) {
	uint32_t v = 0;
	for (int32_t i = 0; i < bytes; i++) {
		v |= static_cast<uint32_t>(b[at + i]) << (8 * i);
	}
	return v;
}

/// A RIFF WAVE file of integer PCM (8 to 32 bits) or 32-bit float, mixed to mono. nullopt for anything else.
std::optional<Sample> readWav(const fs::path& path) {
	std::ifstream in(path, std::ios::binary);
	std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	if (b.size() < 12 || std::string(b.begin(), b.begin() + 4) != "RIFF"
	    || std::string(b.begin() + 8, b.begin() + 12) != "WAVE") {
		return std::nullopt;
	}
	int32_t format = 0;
	int32_t channels = 0;
	int32_t bits = 0;
	Sample sample;
	for (size_t at = 12; at + 8 <= b.size();) {
		std::string id(b.begin() + at, b.begin() + at + 4);
		size_t size = le(b, at + 4, 4);
		size_t body = at + 8;
		if (body + size > b.size()) {
			size = b.size() - body;
		}
		if (id == "fmt " && size >= 16) {
			format = static_cast<int32_t>(le(b, body, 2));
			channels = static_cast<int32_t>(le(b, body + 2, 2));
			sample.rate = static_cast<int32_t>(le(b, body + 4, 4));
			bits = static_cast<int32_t>(le(b, body + 14, 2));
			if (format == 0xFFFE && size >= 26) {
				format = static_cast<int32_t>(le(b, body + 24, 2)); // WAVE_FORMAT_EXTENSIBLE's sub-format
			}
		}
		else if (id == "data") {
			const bool isFloat = format == 3 && bits == 32;
			if ((format != 1 && !isFloat) || channels < 1 || bits < 8 || bits > 32 || bits % 8 != 0
			    || sample.rate <= 0) {
				return std::nullopt;
			}
			const int32_t bytes = bits / 8;
			const size_t frameBytes = static_cast<size_t>(bytes) * channels;
			for (size_t f = body; f + frameBytes <= body + size; f += frameBytes) {
				float sum = 0;
				for (int32_t c = 0; c < channels; c++) {
					uint32_t raw = le(b, f + c * bytes, bytes);
					float v;
					if (isFloat) {
						std::memcpy(&v, &raw, sizeof(v));
					}
					else if (bytes == 1) {
						v = (static_cast<float>(raw) - 128.f) / 128.f; // 8-bit WAV is unsigned
					}
					else {
						int32_t s = static_cast<int32_t>(raw << (32 - bits)); // sign-extend from the top
						v = static_cast<float>(s) / 2147483648.f;
					}
					sum += v;
				}
				sample.frames.push_back(sum / static_cast<float>(channels));
			}
			return sample;
		}
		at = body + size + (size & 1);
	}
	return std::nullopt;
}

// ---- Voices ----

/// One kit row: plays its sample once per hit (or a stand-in hit), each hit cutting the last.
struct KitRow {
	const Sample* sample = nullptr;
	Lane lane = Lane::Kick;
	bool playing = false;
	double position = 0;
	float gain = 0;
	// Stand-in state
	float phase = 0;
	float level = 0;
	int64_t age = 0;
	uint32_t noise = 1;
	float lastNoise = 0;

	void strike(uint8_t velocity) {
		playing = true;
		position = 0;
		gain = kSampleGain * static_cast<float>(velocity) / 127.f;
		phase = 0;
		level = 1;
		age = 0;
		noise = 0x1234567u + static_cast<uint32_t>(lane);
	}

	float next() {
		if (!playing) {
			return 0;
		}
		return sample != nullptr ? nextSample() : nextStandIn();
	}

	float nextSample() {
		const size_t i = static_cast<size_t>(position);
		if (i >= sample->frames.size()) {
			playing = false;
			return 0;
		}
		const float frac = static_cast<float>(position - static_cast<double>(i));
		const float a = sample->frames[i];
		const float b = i + 1 < sample->frames.size() ? sample->frames[i + 1] : 0.f;
		position += static_cast<double>(sample->rate) / kSampleRate;
		return gain * (a + (b - a) * frac);
	}

	float whiteNoise() {
		noise = noise * 1664525u + 1013904223u;
		return static_cast<float>(static_cast<int32_t>(noise)) / 2147483648.f;
	}

	/// A rough drum per lane: a falling sine for the kick, filtered noise with a short decay for everything else.
	float nextStandIn() {
		const float t = static_cast<float>(age++) / kSampleRate;
		float out;
		float decaySeconds;
		switch (lane) {
		case Lane::Kick: {
			const float freq = 50.f + 100.f * std::exp(-t * 30.f);
			phase += kTwoPi * freq / kSampleRate;
			out = std::sin(phase);
			decaySeconds = 0.25f;
			break;
		}
		case Lane::ClosedHat:
		case Lane::OpenHat:
		case Lane::Ride:
		case Lane::Crash: {
			const float n = whiteNoise();
			out = n - lastNoise; // a crude high-pass
			lastNoise = n;
			decaySeconds = lane == Lane::ClosedHat ? 0.04f : lane == Lane::OpenHat ? 0.25f : 0.6f;
			break;
		}
		case Lane::Riser:
		case Lane::Fall:
		case Lane::Impact:
		case Lane::Siren:
			out = whiteNoise();
			decaySeconds = 1.f;
			break;
		default:
			phase += kTwoPi * 220.f / kSampleRate;
			out = 0.5f * whiteNoise() + 0.5f * std::sin(phase);
			decaySeconds = 0.12f;
			break;
		}
		level = std::exp(-t / (decaySeconds / 4.f));
		if (level < 0.0005f) {
			playing = false;
		}
		return gain * out * level;
	}
};

/// The stand-in subtractive voice: a saw through a resonant low-pass whose cutoff falls after each note-on, with an
/// attack-decay-sustain-release amp envelope.
struct SynthVoice {
	uint8_t pitch = 0;
	bool active = false;
	bool releasing = false;
	float phase = 0;
	float increment = 0;
	float velocity = 0;
	float amp = 0;
	int64_t age = 0;
	// State-variable filter
	float low = 0;
	float band = 0;

	void start(uint8_t notePitch, uint8_t noteVelocity) {
		pitch = notePitch;
		active = true;
		releasing = false;
		phase = 0;
		increment = 440.f * std::pow(2.f, (static_cast<float>(notePitch) - 69.f) / 12.f) / kSampleRate;
		velocity = static_cast<float>(noteVelocity) / 127.f;
		age = 0;
		amp = 0;
		low = 0;
		band = 0;
	}

	float next() {
		if (!active) {
			return 0;
		}
		const float t = static_cast<float>(age++) / kSampleRate;
		const float saw = 2.f * phase - 1.f;
		phase += increment;
		if (phase >= 1.f) {
			phase -= 1.f;
		}
		const float cutoff = 300.f + 3000.f * velocity * std::exp(-t * 8.f);
		const float f = 2.f * std::sin(std::numbers::pi_v<float> * std::min(cutoff, 8000.f) / kSampleRate);
		const float q = 0.35f; // fairly resonant
		low += f * band;
		const float high = saw - low - q * band;
		band += f * high;

		if (releasing) {
			amp *= 0.9985f; // about 30 ms to -60 dB
			if (amp < 0.0005f) {
				active = false;
			}
		}
		else {
			const float attack = std::min(1.f, t / 0.003f);
			const float sustain = 0.7f + 0.3f * std::exp(-t * 6.f);
			amp = attack * sustain;
		}
		return kSynthGain * velocity * amp * low;
	}
};

struct PartVoices {
	std::vector<SynthVoice> voices = std::vector<SynthVoice>(8);
	int32_t nextSteal = 0;

	void noteOn(uint8_t pitch, uint8_t velocity) {
		for (SynthVoice& v : voices) {
			if (!v.active) {
				v.start(pitch, velocity);
				return;
			}
		}
		voices[nextSteal].start(pitch, velocity);
		nextSteal = (nextSteal + 1) % static_cast<int32_t>(voices.size());
	}
	void noteOff(uint8_t pitch) {
		for (SynthVoice& v : voices) {
			if (v.active && !v.releasing && v.pitch == pitch) {
				v.releasing = true;
				return;
			}
		}
	}
};

void put16(std::vector<uint8_t>& b, uint16_t v) {
	b.push_back(static_cast<uint8_t>(v));
	b.push_back(static_cast<uint8_t>(v >> 8));
}
void put32(std::vector<uint8_t>& b, uint32_t v) {
	put16(b, static_cast<uint16_t>(v));
	put16(b, static_cast<uint16_t>(v >> 16));
}

} // namespace

Audio renderAudio(const Rendering& rendering, const SoundLibrary& library) {
	const Recipe& recipe = rendering.recipe;
	const Genre& genre = *recipe.genre;
	const int32_t numParts = static_cast<int32_t>(std::min<size_t>(genre.parts.size(), kMaxParts));
	Audio audio;

	// Samples, looked up once per path, root by root.
	std::map<std::string, std::optional<Sample>> samples;
	auto load = [&](const char* path) -> const Sample* {
		if (path == nullptr) {
			return nullptr;
		}
		auto [it, inserted] = samples.try_emplace(path);
		if (inserted) {
			auto found = std::find_if(library.roots.begin(), library.roots.end(), [&](const std::string& root) {
				return fs::is_regular_file(fs::path(root) / path);
			});
			if (found == library.roots.end()) {
				audio.warnings.push_back(std::string(path)
				                         + ": not in pack/ or the card folder; playing a stand-in hit");
			}
			else if (!(it->second = readWav(fs::path(*found) / path))) {
				audio.warnings.push_back(std::string(path) + ": can't read it from " + *found
				                         + " (not PCM or float WAV); playing a stand-in hit");
			}
		}
		return it->second ? &*it->second : nullptr;
	};

	KitRow rows[kNumLanes];
	for (int32_t l = 0; l < kNumLanes; l++) {
		rows[l].lane = static_cast<Lane>(l);
	}
	std::vector<PartVoices> parts(numParts);
	std::vector<bool> warnedPart(numParts, false);

	const int64_t total = frameAt(rendering.bars * kStepsPerBar * kTicksPerStep, recipe.tempo) + kSampleRate;
	std::vector<uint8_t>& wav = audio.wav;
	wav.reserve(44 + total * 4);
	wav.insert(wav.end(), {'R', 'I', 'F', 'F'});
	put32(wav, static_cast<uint32_t>(36 + total * 4));
	wav.insert(wav.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
	put32(wav, 16);
	put16(wav, 1);
	put16(wav, 2);
	put32(wav, kSampleRate);
	put32(wav, kSampleRate * 4);
	put16(wav, 4);
	put16(wav, 16);
	wav.insert(wav.end(), {'d', 'a', 't', 'a'});
	put32(wav, static_cast<uint32_t>(total * 4));

	std::vector<Event> events = rendering.events;
	std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) { return a.tick < b.tick; });
	size_t nextEvent = 0;
	for (int64_t frame = 0; frame < total; frame++) {
		for (; nextEvent < events.size() && frameAt(events[nextEvent].tick, recipe.tempo) <= frame; nextEvent++) {
			const Event& e = events[nextEvent];
			if (e.part >= numParts) {
				continue;
			}
			if (e.lane != Lane::None) {
				if (e.kind == EventKind::NoteOn) {
					KitRow& row = rows[static_cast<int32_t>(e.lane)];
					row.sample = load(recipe.sounds.lane[static_cast<int32_t>(e.lane)]);
					row.strike(e.velocity);
				}
				continue; // one-shots play out; their note-offs do nothing
			}
			if (e.kind == EventKind::NoteOn) {
				if (!warnedPart[e.part]) {
					warnedPart[e.part] = true;
					const char* preset = recipe.sounds.part[e.part];
					audio.warnings.push_back(std::string(genre.parts[e.part].name) + ": "
					                         + (preset != nullptr ? preset : "no preset")
					                         + ": preset parameters aren't modelled, ignored; playing the stand-in "
					                           "subtractive voice");
				}
				parts[e.part].noteOn(e.pitch, e.velocity);
			}
			else if (e.kind == EventKind::NoteOff) {
				parts[e.part].noteOff(e.pitch);
			}
		}
		float mix = 0;
		for (KitRow& row : rows) {
			mix += row.next();
		}
		for (PartVoices& part : parts) {
			for (SynthVoice& v : part.voices) {
				mix += v.next();
			}
		}
		const int32_t s = static_cast<int32_t>(std::lround(std::clamp(mix, -1.f, 1.f) * 32767.f));
		put16(wav, static_cast<uint16_t>(s));
		put16(wav, static_cast<uint16_t>(s));
	}
	if (recipe.swing != 0) {
		audio.warnings.push_back("swing isn't modelled: the render plays straight");
	}
	return audio;
}

} // namespace harness::render
