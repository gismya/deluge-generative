// The harness's store for steering logs (spec §17.2): what the firmware's card-log task does with the card, in memory.
// It takes each piece's image out of the log's RAM as the consumer does, and reads pieces back whole, the playing one
// too, for specs that look at what was logged.
#pragma once

#include "generative/core/steering_log.h"
#include <climits>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <vector>

namespace harness {

using namespace deluge::generative;

/// A piece read back: its image, which the Piece reads in place.
struct PieceImage {
	std::vector<uint8_t> bytes;
	[[nodiscard]] SteeringLog::Piece piece() const { return SteeringLog::Piece(bytes.data()); }
};

/// Stores what a log's consumer hands over, by piece id. Knows the log it was made for: harness::image() and
/// drainLog() find it from the log (one store a log).
class MemoryLogStore {
public:
	explicit MemoryLogStore(SteeringLog& log) : log_(log) { registry()[&log] = this; }
	~MemoryLogStore() { registry().erase(&log_); }
	MemoryLogStore(const MemoryLogStore&) = delete;
	MemoryLogStore& operator=(const MemoryLogStore&) = delete;

	/// Takes everything ready, as the card-log task would, at most `maxBytes` (a stall: less). Pieces whose store
	/// fails (failFrom) take no more bytes past it, and the log hears of it once.
	void drain(int32_t maxBytes = INT_MAX) {
		if (stalled) {
			return;
		}
		LogChunk chunk;
		while (maxBytes > 0 && log_.nextChunk(chunk, maxBytes)) {
			if (chunk.data != nullptr && failing(chunk)) {
				if (!failed_.contains(chunk.id)) {
					failed_.insert(chunk.id);
					log_.storeFailed(chunk.slot);
				}
			}
			else if (chunk.data != nullptr) {
				std::vector<uint8_t>& file = files[chunk.id];
				if (chunk.offset == 0) {
					PieceInfo info;
					log_.imageInfo(chunk.slot, info);
					const auto* at = reinterpret_cast<const uint8_t*>(&info);
					file.assign(at, at + sizeof info);
				}
				else {
					file.insert(file.end(), chunk.data, chunk.data + chunk.length);
				}
			}
			maxBytes -= chunk.length;
			log_.took(chunk);
		}
		for (int32_t s = 0; s < kMaxLogPieces; s++) {
			if (log_.used(s) && log_.allTaken(s)) {
				if (log_.dropped(s)) {
					files.erase(log_.idOf(s));
					log_.released(s);
				}
			}
			// Its info into its file again, as the task does at each flush and at f_close.
			if (log_.used(s) && !log_.dropped(s) && files.contains(log_.idOf(s)) && !failed_.contains(log_.idOf(s))) {
				PieceInfo info;
				log_.imageInfo(s, info);
				std::memcpy(files[log_.idOf(s)].data(), &info, sizeof info);
			}
		}
	}

	/// Piece `i` of the log, whole: what the store has, the bytes still in RAM after it, and its info as the table
	/// knows it now (the genre's pointer, marks).
	[[nodiscard]] PieceImage image(int32_t i) {
		drain();
		const int32_t slot = log_.slotOf(i);
		const SteeringLog::PieceSummary summary = log_.piece(i);
		PieceImage out;
		out.bytes.resize(static_cast<size_t>(std::max<int32_t>(summary.bytes(), sizeof(PieceInfo))));
		const auto found = files.find(summary.id());
		int32_t have = 0;
		if (found != files.end()) {
			have = std::min<int32_t>(static_cast<int32_t>(found->second.size()), summary.bytes());
			std::memcpy(out.bytes.data(), found->second.data(), static_cast<size_t>(have));
		}
		log_.copyUntaken(slot, have, out.bytes.data() + have);
		std::memcpy(out.bytes.data(), &summary.info(), sizeof(PieceInfo));
		return out;
	}

	/// Piece `i` as Capture reads it after Perform: its file alone, through SteeringLog::readImage(). Empty if the
	/// store has none, or readImage() refuses it.
	[[nodiscard]] PieceImage stored(int32_t i) {
		drain();
		PieceImage out;
		const auto found = files.find(log_.piece(i).id());
		if (found != files.end()) {
			out.bytes = found->second;
			if (!SteeringLog::readImage(out.bytes.data(), static_cast<int32_t>(out.bytes.size()))) {
				out.bytes.clear();
			}
		}
		return out;
	}

	/// The store of `log`, or nullptr.
	static MemoryLogStore* of(const SteeringLog* log) {
		const auto found = registry().find(log);
		return found != registry().end() ? found->second : nullptr;
	}

	/// The files, by piece id: each image as stored.
	std::map<uint32_t, std::vector<uint8_t>> files;
	/// The card stalls: nothing drains until it is false again.
	bool stalled = false;
	/// A store failure from this many bytes of any piece's image on (a card error, a full card): -1 for none.
	int32_t failFrom = -1;

private:
	bool failing(const LogChunk& chunk) const {
		return failed_.contains(chunk.id) || (failFrom >= 0 && chunk.offset + chunk.length > failFrom);
	}
	static std::map<const SteeringLog*, MemoryLogStore*>& registry() {
		static std::map<const SteeringLog*, MemoryLogStore*> stores;
		return stores;
	}
	SteeringLog& log_;
	std::set<uint32_t> failed_;
};

/// Drains `log`'s store, if it has one: what the card-log task does between ticks.
inline void drainLog(const SteeringLog* log) {
	if (MemoryLogStore* store = MemoryLogStore::of(log)) {
		store->drain();
	}
}

/// Piece `i` of `log` read back whole (`log` must have a store).
inline PieceImage image(const SteeringLog& log, int32_t i) {
	return MemoryLogStore::of(&log)->image(i);
}

/// A log over Perform's sizes with its store: the bytes, the log, the store.
struct StoredLog {
	explicit StoredLog(int32_t headerBytes = kLogHeaderBytes, int32_t ringBytes = kLogRingBytes)
	    : bytes(static_cast<size_t>(SteeringLog::bytesFor(headerBytes, ringBytes))),
	      log(bytes.data(), headerBytes, ringBytes), store(log) {}
	StoredLog(const StoredLog&) = delete;
	StoredLog& operator=(const StoredLog&) = delete;
	std::vector<uint8_t> bytes;
	SteeringLog log;
	MemoryLogStore store;
};

} // namespace harness
