// Shared by the Compose and Capture specs (compose_driver_spec, capture_replay_spec): a SongWriter that records, and
// Perform's played stream as notes and automation curves to hold a song's against.

#pragma once

#include "generative/compose/clip_builder.h"
#include "harness.h"

#include <algorithm>
#include <array>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace harness {

using namespace deluge::generative::compose;

/// Keeps what Compose writes, in call order.
struct RecordingWriter final : SongWriter {
	const Recipe* recipe = nullptr;
	std::vector<Note> notes;
	std::vector<std::pair<Automated, Node>> nodes;
	std::vector<std::pair<Automated, int8_t>> bases;
	int32_t endTick = -1;
	uint32_t soundingParts = 0;
	int32_t calls = 0;
	int32_t endCall = -1;
	int32_t riserMicros = 0;
	void begin(const Recipe& r) override {
		recipe = &r;
		calls++;
	}
	void note(const Note& n) override {
		notes.push_back(n);
		calls++;
	}
	void node(const Automated& a, const Node& n) override {
		nodes.push_back({a, n});
		lateNodes += n.tick < cutAt[a.part] ? 1 : 0;
		calls++;
	}
	void base(const Automated& a, int8_t value) override {
		bases.push_back({a, value});
		calls++;
	}
	void end(int32_t tick, uint32_t sounding) override {
		endTick = tick;
		soundingParts = sounding;
		endCall = calls++;
	}
	int32_t sampleMicros(const Event& /*noteOn*/) override { return riserMicros; }

	// Capture's calls.
	struct Pick {
		uint8_t part;
		uint8_t token;
		int32_t slot;
		std::string path;
		bool operator==(const Pick&) const = default;
	};
	struct Track {
		uint8_t part;
		uint8_t token;
		int32_t tick;
		bool operator==(const Track&) const = default;
	};
	std::vector<std::pair<uint8_t, int32_t>> cuts; ///< (part, tick)
	std::vector<Pick> picks;
	std::vector<Track> tracks;
	std::vector<TempoNode> tempos;
	std::vector<std::pair<Command, int32_t>> edits;
	std::vector<std::pair<bool, int32_t>> stutters;
	std::vector<int32_t> songs; ///< nextSong() ticks
	/// Each part's last clip cut or new track, and the nodes that came for the part after it with a tick before it.
	std::array<int32_t, kMaxParts> cutAt = [] {
		std::array<int32_t, kMaxParts> none{};
		none.fill(INT32_MIN);
		return none;
	}();
	int32_t lateNodes = 0;
	void cutClip(uint8_t part, int32_t tick) override {
		cuts.emplace_back(part, tick);
		cutAt[part] = tick;
	}
	void soundPick(uint8_t part, uint8_t token, int32_t slot, const char* path, int32_t length) override {
		picks.push_back({part, token, slot, std::string(path, length)});
	}
	void newTrack(uint8_t part, uint8_t token, int32_t tick) override {
		tracks.push_back({part, token, tick});
		cutAt[part] = tick;
	}
	void tempo(const TempoNode& node) override { tempos.push_back(node); }
	std::vector<size_t> editNodes; ///< nodes.size() at each paramEdit()
	void paramEdit(const Command& edit, int32_t tick) override {
		edits.emplace_back(edit, tick);
		editNodes.push_back(nodes.size());
	}
	void stutter(bool on, int32_t tick) override { stutters.emplace_back(on, tick); }
	void nextSong(int32_t tick) override { songs.push_back(tick); }

	[[nodiscard]] std::vector<Node> nodesOf(const Automated& a) const {
		std::vector<Node> out;
		for (const auto& [id, node] : nodes) {
			if (id == a) {
				out.push_back(node);
			}
		}
		return out;
	}
};

/// Perform's played stream paired into notes, as Compose pairs Compose's: the note-on's tick, to its note-off.
inline std::vector<Note> pairNotes(const std::vector<Event>& events) {
	std::vector<Note> out;
	std::vector<Event> open;
	for (const Event& e : events) {
		if (e.kind == EventKind::NoteOn && !e.muted) {
			open.push_back(e);
		}
		else if (e.kind == EventKind::NoteOff) {
			auto found = std::find_if(open.begin(), open.end(), [&](const Event& o) { return o.sameKey(e); });
			if (found != open.end()) {
				out.push_back(
				    {found->tick, e.tick - found->tick, found->part, found->lane, found->pitch, found->velocity});
				open.erase(found);
			}
		}
	}
	return out;
}

/// Notes that start before `cutTick` less the most a note leans early, so only the decisions before the cut's own
/// made them, ended no later than `cutTick`, sorted: what both runs agree on.
inline std::vector<Note> notesBefore(std::vector<Note> notes, int32_t cutTick) {
	std::vector<Note> out;
	for (Note n : notes) {
		if (n.tick < cutTick - kMaxLean) {
			n.length = std::min(n.tick + n.length, cutTick) - n.tick;
			out.push_back(n);
		}
	}
	std::sort(out.begin(), out.end(), [](const Note& a, const Note& b) {
		return std::tie(a.tick, a.part, a.lane, a.pitch, a.length, a.velocity)
		       < std::tie(b.tick, b.part, b.lane, b.pitch, b.length, b.velocity);
	});
	return out;
}

using LaneKey = std::tuple<uint8_t, Lane, SoundControl, bool>;
inline LaneKey keyOf(const Automated& a) {
	return {a.part, a.lane, a.control, a.throwDelay};
}

/// The value each automation lane has on each step line before `cut`, as Perform plays its Control events: a snap
/// jumps on its line, a ramp reaches its value on the next. Every lane starts at home, 0.
inline std::map<LaneKey, std::vector<int32_t>> performCurves(const std::vector<Event>& events, int32_t cut) {
	std::map<LaneKey, std::vector<int32_t>> out;
	auto apply = [&](const Automated& a, int32_t value, const Event& e) {
		auto [entry, added] = out.try_emplace(keyOf(a), std::vector<int32_t>(cut + 1, 0));
		std::vector<int32_t>& curve = entry->second;
		const int32_t step = e.tick / kTicksPerStep;
		for (int32_t s = e.snap ? step : step + 1; s <= cut; s++) {
			curve[s] = value;
		}
	};
	for (const Event& e : events) {
		if (e.kind != EventKind::Control || e.tick >= stepTick(cut)) {
			continue;
		}
		apply({e.part, e.lane, e.control, false}, e.offset, e);
		if (e.control == SoundControl::Space) {
			apply({e.part, e.lane, e.control, true}, e.throwDelay(), e);
		}
	}
	return out;
}

/// What a lane's nodes play at `tick`, as the firmware's automation does: a straight line into an interpolated node,
/// a jump onto any other; `base` with no node. `from` is a node index at or before `tick`'s, kept from call to call as
/// ticks rise.
inline int32_t nodeValue(const std::vector<Node>& nodes, int32_t base, int32_t tick, size_t& from) {
	if (nodes.empty()) {
		return base;
	}
	while (from + 1 < nodes.size() && nodes[from + 1].tick <= tick) {
		from++;
	}
	const Node& a = nodes[from];
	if (from + 1 == nodes.size() || tick <= a.tick) {
		return a.value;
	}
	const Node& b = nodes[from + 1];
	return b.interpolated ? a.value + (b.value - a.value) * (tick - a.tick) / (b.tick - a.tick) : a.value;
}

} // namespace harness
