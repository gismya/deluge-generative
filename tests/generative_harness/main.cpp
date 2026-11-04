/*
 * Copyright © 2026 Lars Johansson
 *
 * This file is part of The Synthstrom Audible Deluge Firmware.
 *
 * The Synthstrom Audible Deluge Firmware is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

// Desktop driver for the Generator core: runs it for N bars, prints the event stream and optionally writes a
// Standard MIDI File. See README.md.

#include "generative/core/generator.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace deluge::generative;

namespace {

constexpr const char* kLaneNames[kNumLanes] = {"Kick", "Clap", "ClosedHat", "OpenHat"};
constexpr uint8_t kGmDrumNotes[kNumLanes] = {36, 39, 42, 46}; // Bass drum 1, hand clap, closed hat, open hat.
constexpr const char* kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
constexpr int32_t kPpqn = 96;

struct Captured {
	int32_t tick;
	PartId part;
	uint8_t lane;
	int16_t note;
	uint8_t velocity; // 0 = note-off.
	int32_t length;   // Note-ons only, filled in when the matching off arrives. -1 if it never did.
};

class CaptureSink : public EventSink {
public:
	std::vector<Captured> events;

	void noteOn(int32_t tick, PartId part, uint8_t lane, int16_t note, uint8_t velocity) override {
		events.push_back({tick, part, lane, note, velocity, -1});
	}
	void noteOff(int32_t tick, PartId part, uint8_t lane, int16_t note) override {
		for (size_t i = events.size(); i-- > 0;) {
			Captured& e = events[i];
			if (e.velocity != 0 && e.length < 0 && e.part == part
			    && (part == PartId::Drums ? e.lane == lane : e.note == note)) {
				e.length = tick - e.tick;
				break;
			}
		}
		events.push_back({tick, part, lane, note, 0, 0});
	}
};

std::string noteName(int32_t note) {
	return std::string(kNoteNames[note % 12]) + std::to_string(note / 12 - 1);
}

/// "bar.beat.16th", 1-based, for the step whose grid line is nearest the tick, plus the Lean from that line.
void position(int32_t tick, char* out, size_t size, int32_t& lean) {
	int32_t step = (tick + kTicksPerStep / 2) / kTicksPerStep;
	lean = tick - step * kTicksPerStep;
	std::snprintf(out, size, "%3d.%d.%d", step / kStepsPerBar + 1, (step % kStepsPerBar) / 4 + 1, step % 4 + 1);
}

// ---- Standard MIDI File ----

void putVarLen(std::vector<uint8_t>& out, uint32_t v) {
	uint8_t bytes[5];
	int n = 0;
	bytes[n++] = v & 0x7f;
	while (v >>= 7) {
		bytes[n++] = static_cast<uint8_t>((v & 0x7f) | 0x80);
	}
	while (n-- > 0) {
		out.push_back(bytes[n]);
	}
}

void put32(std::vector<uint8_t>& out, uint32_t v) {
	for (int s = 24; s >= 0; s -= 8) {
		out.push_back(static_cast<uint8_t>(v >> s));
	}
}

bool writeMidi(const char* path, const std::vector<Captured>& events, int32_t bpm, int32_t endTick) {
	std::vector<uint8_t> track;
	auto meta = [&](uint8_t type, const std::vector<uint8_t>& data) {
		putVarLen(track, 0);
		track.push_back(0xff);
		track.push_back(type);
		putVarLen(track, static_cast<uint32_t>(data.size()));
		track.insert(track.end(), data.begin(), data.end());
	};
	const char* name = "Generative harness";
	meta(0x03, std::vector<uint8_t>(name, name + std::strlen(name)));
	uint32_t usPerQuarter = 60000000u / static_cast<uint32_t>(bpm);
	meta(0x51, {static_cast<uint8_t>(usPerQuarter >> 16), static_cast<uint8_t>(usPerQuarter >> 8),
	            static_cast<uint8_t>(usPerQuarter)});
	meta(0x58, {4, 2, 24, 8});
	// Bass on channel 1: GM program 39, Synth Bass 1.
	putVarLen(track, 0);
	track.push_back(0xc0);
	track.push_back(38);

	int32_t last = 0;
	for (const Captured& e : events) {
		putVarLen(track, static_cast<uint32_t>(e.tick - last));
		last = e.tick;
		bool drums = e.part == PartId::Drums;
		uint8_t channel = drums ? 9 : 0;
		uint8_t note = drums ? kGmDrumNotes[e.lane] : static_cast<uint8_t>(e.note);
		track.push_back(static_cast<uint8_t>((e.velocity ? 0x90 : 0x80) | channel));
		track.push_back(note);
		track.push_back(e.velocity ? e.velocity : 64);
	}
	// End of track on the last bar line, so the file loops cleanly.
	putVarLen(track, static_cast<uint32_t>(endTick > last ? endTick - last : 0));
	track.insert(track.end(), {0xff, 0x2f, 0x00});

	std::vector<uint8_t> file = {'M', 'T', 'h', 'd'};
	put32(file, 6);
	file.insert(file.end(), {0, 0, 0, 1, 0, kPpqn});
	file.insert(file.end(), {'M', 'T', 'r', 'k'});
	put32(file, static_cast<uint32_t>(track.size()));
	file.insert(file.end(), track.begin(), track.end());

	FILE* f = std::fopen(path, "wb");
	if (!f) {
		return false;
	}
	bool ok = std::fwrite(file.data(), 1, file.size(), f) == file.size();
	return std::fclose(f) == 0 && ok;
}

// ---- Arguments ----

void usage() {
	std::fprintf(stderr, "usage: generative_harness [options]\n"
	                     "  --seed N        seed (default 1)\n"
	                     "  --bars N        bars to run (default 16)\n"
	                     "  --key K         root: 0-11 or a name like C, F#, Bb (default C)\n"
	                     "  --scale S       minor | phrygian | dorian (default minor)\n"
	                     "  --energy N      0-50 (default 25)\n"
	                     "  --density N     0-50 (default 25)\n"
	                     "  --motion N      0-50 (default 25)\n"
	                     "  --bpm N         tempo written to the MIDI file (default 130)\n"
	                     "  --midi PATH     also write a Standard MIDI File (drums ch10 GM notes, bass ch1, 96 PPQN)\n"
	                     "  --offs          print note-off lines too\n"
	                     "  --quiet         print only the summary\n");
}

bool parseKey(const char* s, uint8_t& key) {
	char* end = nullptr;
	long v = std::strtol(s, &end, 10);
	if (end != s && *end == '\0') {
		if (v < 0 || v > 11) {
			return false;
		}
		key = static_cast<uint8_t>(v);
		return true;
	}
	static constexpr int8_t kLetters[7] = {9, 11, 0, 2, 4, 5, 7}; // A B C D E F G
	char c = s[0];
	if (c >= 'a' && c <= 'g') {
		c = static_cast<char>(c - 'a' + 'A');
	}
	if (c < 'A' || c > 'G') {
		return false;
	}
	int32_t pc = kLetters[c - 'A'];
	if (s[1] == '#') {
		pc++;
	}
	else if (s[1] == 'b') {
		pc--;
	}
	key = static_cast<uint8_t>((pc + 12) % 12);
	return true;
}

} // namespace

int main(int argc, char** argv) {
	Recipe recipe;
	uint32_t seed = 1;
	int32_t bars = 16;
	int32_t bpm = 130;
	const char* midiPath = nullptr;
	bool printOffs = false;
	bool quiet = false;

	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		auto value = [&]() -> const char* {
			if (i + 1 >= argc) {
				usage();
				std::exit(2);
			}
			return argv[++i];
		};
		auto macro = [&]() {
			long v = std::strtol(value(), nullptr, 10);
			return static_cast<uint8_t>(v < 0 ? 0 : (v > kMacroMax ? kMacroMax : v));
		};
		if (a == "--seed") {
			seed = static_cast<uint32_t>(std::strtoul(value(), nullptr, 0));
		}
		else if (a == "--bars") {
			bars = static_cast<int32_t>(std::strtol(value(), nullptr, 10));
		}
		else if (a == "--key") {
			if (!parseKey(value(), recipe.key)) {
				usage();
				return 2;
			}
		}
		else if (a == "--scale") {
			std::string s = value();
			if (s == "minor") {
				recipe.scale = Scale::Minor;
			}
			else if (s == "phrygian") {
				recipe.scale = Scale::Phrygian;
			}
			else if (s == "dorian") {
				recipe.scale = Scale::Dorian;
			}
			else {
				usage();
				return 2;
			}
		}
		else if (a == "--energy") {
			recipe.energy = macro();
		}
		else if (a == "--density") {
			recipe.density = macro();
		}
		else if (a == "--motion") {
			recipe.motion = macro();
		}
		else if (a == "--bpm") {
			bpm = static_cast<int32_t>(std::strtol(value(), nullptr, 10));
		}
		else if (a == "--midi") {
			midiPath = value();
		}
		else if (a == "--offs") {
			printOffs = true;
		}
		else if (a == "--quiet") {
			quiet = true;
		}
		else {
			usage();
			return a == "--help" || a == "-h" ? 0 : 2;
		}
	}
	if (bars < 1 || bpm < 20 || bpm > 999) {
		usage();
		return 2;
	}

	// Drive the core the way Perform does when it honours the returned waits.
	static Generator gen; // Static only to keep the desktop stack small; the core itself has no globals.
	CaptureSink sink;
	gen.start(recipe, seed);
	int32_t end = bars * kStepsPerBar * kTicksPerStep;
	int32_t now = 0;
	while (now < end) {
		now += gen.tick(now, sink);
	}
	gen.stop(sink);

	static constexpr const char* kScaleNames[] = {"minor", "phrygian", "dorian"};
	std::printf("# seed %u, %d bars, key %s %s, energy %u, density %u, motion %u (sizeof(Generator) = %zu bytes)\n",
	            seed, bars, kNoteNames[recipe.key], kScaleNames[static_cast<int>(recipe.scale)], recipe.energy,
	            recipe.density, recipe.motion, sizeof(Generator));

	int32_t counts[kNumLanes + 1] = {};
	int32_t lowest = 127;
	int32_t highest = 0;
	for (const Captured& e : sink.events) {
		if (e.velocity != 0) {
			if (e.part == PartId::Drums) {
				counts[e.lane]++;
			}
			else {
				counts[kNumLanes]++;
				lowest = e.note < lowest ? e.note : lowest;
				highest = e.note > highest ? e.note : highest;
			}
		}
		if (quiet || (e.velocity == 0 && !printOffs)) {
			continue;
		}
		char pos[48];
		int32_t lean;
		position(e.tick, pos, sizeof(pos), lean);
		bool drums = e.part == PartId::Drums;
		const char* lane = drums ? kLaneNames[e.lane] : "-";
		std::string note = drums ? "-" : std::to_string(e.note) + " " + noteName(e.note);
		if (e.velocity != 0) {
			std::printf("%s  tick %6d %+3d  %-5s %-9s  note %-7s vel %3u  len %d\n", pos, e.tick, lean,
			            drums ? "Drums" : "Bass", lane, note.c_str(), e.velocity, e.length);
		}
		else {
			std::printf("%s  tick %6d %+3d  %-5s %-9s  note %-7s off\n", pos, e.tick, lean, drums ? "Drums" : "Bass",
			            lane, note.c_str());
		}
	}
	std::printf("# note-ons: Kick %d, Clap %d, ClosedHat %d, OpenHat %d, Bass %d", counts[0], counts[1], counts[2],
	            counts[3], counts[4]);
	if (counts[4] > 0) {
		std::printf(" (bass range %s..%s)", noteName(lowest).c_str(), noteName(highest).c_str());
	}
	std::printf("\n");

	if (midiPath) {
		if (!writeMidi(midiPath, sink.events, bpm, end)) {
			std::fprintf(stderr, "could not write %s\n", midiPath);
			return 1;
		}
		std::printf("# wrote %s at %d BPM\n", midiPath, bpm);
	}
	return 0;
}
