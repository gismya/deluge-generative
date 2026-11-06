// Rough render: the Standard MIDI File.

#include "render/render.h"

#include "generative/core/genre.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace harness::render {

namespace {

constexpr int32_t kTicksPerQuarter = 4 * kTicksPerStep; // 96
constexpr uint8_t kDrumChannel = 9;

/// GM drum notes per lane, in Lane order. The FX lanes have no GM note, so they sit above the drum map.
constexpr uint8_t kLaneNote[kNumLanes] = {
    36, // Kick
    38, // Snare
    39, // Clap
    37, // Rim
    42, // ClosedHat
    46, // OpenHat
    51, // Ride
    49, // Crash
    45, // Tom
    60, // Perc1: hi bongo
    61, // Perc2: low bongo
    84, // Riser
    85, // Fall
    86, // Impact
    87, // Siren
};

constexpr const char* kSectionName[kNumSectionKinds] = {"INTRO", "GROOVE", "BREAK", "BUILD", "OUTRO"};

class Track {
public:
	void meta(int32_t tick, uint8_t type, const void* data, size_t length) {
		delta(tick);
		bytes_.push_back(0xFF);
		bytes_.push_back(type);
		vlq(static_cast<uint32_t>(length));
		const auto* p = static_cast<const uint8_t*>(data);
		bytes_.insert(bytes_.end(), p, p + length);
	}
	void text(int32_t tick, uint8_t type, const char* s) { meta(tick, type, s, std::strlen(s)); }
	void message(int32_t tick, uint8_t status, uint8_t a, uint8_t b) {
		delta(tick);
		bytes_.push_back(status);
		bytes_.push_back(a);
		bytes_.push_back(b);
	}
	void appendTo(std::vector<uint8_t>& out, int32_t endTick) {
		meta(endTick, 0x2F, nullptr, 0);
		const uint8_t header[] = {'M', 'T', 'r', 'k'};
		out.insert(out.end(), header, header + 4);
		be32(out, static_cast<uint32_t>(bytes_.size()));
		out.insert(out.end(), bytes_.begin(), bytes_.end());
	}

	static void be32(std::vector<uint8_t>& out, uint32_t v) {
		for (int32_t shift = 24; shift >= 0; shift -= 8) {
			out.push_back(static_cast<uint8_t>(v >> shift));
		}
	}

private:
	void delta(int32_t tick) {
		tick = std::max(tick, last_);
		vlq(static_cast<uint32_t>(tick - last_));
		last_ = tick;
	}
	void vlq(uint32_t v) {
		uint8_t buffer[5];
		int32_t n = 0;
		do {
			buffer[n++] = static_cast<uint8_t>(v & 0x7F);
			v >>= 7;
		} while (v != 0);
		while (n > 0) {
			n--;
			bytes_.push_back(static_cast<uint8_t>(buffer[n] | (n > 0 ? 0x80 : 0)));
		}
	}

	std::vector<uint8_t> bytes_;
	int32_t last_ = 0;
};

} // namespace

std::vector<uint8_t> midiFile(const Rendering& rendering) {
	const Genre& genre = *rendering.recipe.genre;
	const int32_t numParts = static_cast<int32_t>(std::min<size_t>(genre.parts.size(), kMaxParts));

	Track conductor;
	const uint32_t usPerQuarter = static_cast<uint32_t>(6'000'000'000LL / rendering.recipe.tempo);
	const uint8_t tempo[] = {static_cast<uint8_t>(usPerQuarter >> 16), static_cast<uint8_t>(usPerQuarter >> 8),
	                         static_cast<uint8_t>(usPerQuarter)};
	conductor.meta(0, 0x51, tempo, sizeof(tempo));
	const uint8_t fourFour[] = {4, 2, 24, 8};
	conductor.meta(0, 0x58, fourFour, sizeof(fourFour));

	std::vector<Track> parts(numParts);
	for (int32_t p = 0; p < numParts; p++) {
		parts[p].text(0, 0x03, genre.parts[p].name);
	}

	// Play order within a tick, time order across ticks.
	std::vector<Event> events = rendering.events;
	std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) { return a.tick < b.tick; });
	for (const Event& e : events) {
		const int32_t tick = std::max(e.tick, 0);
		if (e.kind == EventKind::Section) {
			char marker[16];
			std::snprintf(marker, sizeof(marker), "%s %d", kSectionName[static_cast<int32_t>(e.section)],
			              e.occurrence + 1);
			conductor.text(tick, 0x06, marker);
			continue;
		}
		if ((e.kind != EventKind::NoteOn && e.kind != EventKind::NoteOff) || e.part >= numParts) {
			continue;
		}
		const bool kit = e.lane != Lane::None;
		const uint8_t channel = kit ? kDrumChannel : static_cast<uint8_t>(e.part < kDrumChannel ? e.part : e.part + 1);
		const uint8_t note = kit ? kLaneNote[static_cast<int32_t>(e.lane)] : e.pitch;
		if (e.kind == EventKind::NoteOn) {
			parts[e.part].message(tick, 0x90 | channel, note, e.velocity);
		}
		else {
			parts[e.part].message(tick, 0x80 | channel, note, 0x40);
		}
	}

	std::vector<uint8_t> out = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1};
	const uint16_t numTracks = static_cast<uint16_t>(numParts + 1);
	out.push_back(static_cast<uint8_t>(numTracks >> 8));
	out.push_back(static_cast<uint8_t>(numTracks));
	out.push_back(static_cast<uint8_t>(kTicksPerQuarter >> 8));
	out.push_back(static_cast<uint8_t>(kTicksPerQuarter));
	conductor.appendTo(out, 0);
	for (Track& track : parts) {
		track.appendTo(out, 0);
	}
	return out;
}

} // namespace harness::render
