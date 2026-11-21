// The steering log (spec §17.2, §17.3): its records, piece headers and pieces, in the buffer and as the Perform
// driver writes them.

#include "generative/core/steering_log.h"
#include "harness.h"
#include "stream_helpers.h"
#include "transition_piece.h"

#include "cppspec.hpp"

#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using namespace harness;

namespace {

/// A log with a ring of `ring` bytes (and a header area of `header`) and its store, with nothing in it. Nothing drains
/// unless the spec says so (store.drain()), so appending without one fills the ring as a card stall would.
struct Buffer : StoredLog {
	explicit Buffer(int32_t ring, int32_t header = kLogHeaderBytes) : StoredLog(header, ring) {}
};

std::vector<LogRecord> records(const SteeringLog& log, int32_t piece) {
	// The images stay for the spec's run: a SoundPath's path points into one.
	static std::vector<std::unique_ptr<PieceImage>> images;
	images.push_back(std::make_unique<PieceImage>(image(log, piece)));
	std::vector<LogRecord> out;
	images.back()->piece().forEachRecord([&](const LogRecord& record) { out.push_back(record); });
	return out;
}

Command macro(Macro which, int32_t value) {
	return {CommandKind::SetMacro, static_cast<uint8_t>(which), value};
}

PieceStart startOf(uint32_t seed) {
	PieceStart start;
	start.recipe = acidRecipe();
	start.seed = seed;
	return start;
}

/// Three marked pieces, seeds 1 to 3, each with a pick and two records, ended.
void threeMarked(SteeringLog& log) {
	for (uint32_t seed = 1; seed <= 3; seed++) {
		drainLog(&log); // the last piece's header is on the card before the next opens
		log.beginPiece(startOf(seed));
		log.addPick(pickSlot(1), "SYNTHS/GEN ACID");
		log.append(2, macro(Macro::Energy, static_cast<int32_t>(seed)));
		log.append(5, macro(Macro::Density, static_cast<int32_t>(seed)));
		log.mark(true);
		log.endPiece(8, PieceEnd::Stopped);
	}
}
/// Each piece's seed, in order.
std::vector<uint32_t> seeds(const SteeringLog& log) {
	std::vector<uint32_t> out;
	for (int32_t i = 0; i < log.pieces(); i++) {
		out.push_back(log.piece(i).start().seed);
	}
	return out;
}
/// Whether threeMarked()'s pieces from piece `first` on (two of them) have their pick and records as logged.
bool twoIntact(const SteeringLog& log, int32_t first) {
	bool intact = true;
	for (int32_t i = first; i < first + 2; i++) {
		const auto seed = static_cast<int32_t>(log.piece(i).start().seed);
		char path[kMaxSoundPath];
		intact = intact && image(log, i).piece().pick(pickSlot(1), path) && std::string{path} == "SYNTHS/GEN ACID";
		const std::vector<LogRecord> read = records(log, i);
		intact = intact && read.size() == 2 && read[0].command == macro(Macro::Energy, seed) && read[0].step == 2
		         && read[1].command == macro(Macro::Density, seed) && read[1].step == 5
		         && log.piece(i).recordCount() == 2;
	}
	return intact;
}
/// Every kind, with values at the edges of what each field holds.
std::vector<Command> everyCommand() {
	std::vector<Command> out;
	for (int32_t k = 0; k <= static_cast<int32_t>(kLastCommandKind); k++) {
		const auto kind = static_cast<CommandKind>(k);
		for (int32_t value :
		     std::initializer_list<int32_t>{0, 1, -1, 63, -64, 64, 300, -300, std::numeric_limits<int32_t>::max(),
		                                    std::numeric_limits<int32_t>::min()}) {
			out.push_back({kind, static_cast<uint8_t>(k * 37), value});
		}
		out.push_back({kind, 255, 5, 65535, Lane::Riser});
		out.push_back({kind, 0, -2, 0, Lane::Kick});
		out.push_back({kind, 3, 12800, 1, Lane::None});
	}
	return out;
}

/// Records the header and paths the driver asks for, as the firmware's OutputSink writes them.
struct HeaderSink final : perform::NoteSink {
	std::vector<Event> events;
	int32_t headers = 0;
	void play(const Event& event) override { events.push_back(event); }
	void logPieceHeader(SteeringLog& log) override {
		headers++;
		log.addPick(pickSlot(1), "SYNTHS/GEN ACID");
		const int32_t snapshot[3] = {headers, -7, 1 << 30};
		log.addSnapshot(snapshot, sizeof snapshot);
	}
	void logSoundPaths(SteeringLog& log, int32_t step, uint8_t part, uint8_t token) override {
		log.appendSoundPath(step, part, token, pickSlot(part), "SYNTHS/GEN NEW");
	}
	/// A restart's line starts the piece again, as RecordingSink's does: a StepwisePiece sets it.
	std::function<void(Generator&)> restarts;
	void restartPiece(Generator& generator) override {
		if (restarts) {
			restarts(generator);
		}
	}
};

} // namespace

// clang-format off
describe steering_log("The steering log", $ {
	context("records", _ {
		it("reads back every command as it was logged, with its step", _ {
			Buffer b(64 * 1024);
			b.log.beginPiece(startOf(1));
			const std::vector<Command> commands = everyCommand();
			for (size_t i = 0; i < commands.size(); i++) {
				expect(b.log.append(static_cast<int32_t>(i / 3) * 1000, commands[i])).to_be_true();
			}
			const std::vector<LogRecord> read = records(b.log, 0);
			expect(read.size()).to_equal(commands.size());
			bool same = true;
			for (size_t i = 0; i < read.size(); i++) {
				same = same && read[i].kind == RecordKind::Command && read[i].command == commands[i]
				       && read[i].step == static_cast<int32_t>(i / 3) * 1000;
			}
			expect(same).to_be_true();
		});

		it("packs a macro turned a knob step at a time into 5 bytes a step at most", _ {
			uint8_t out[kMaxCommandBytes];
			int32_t most = 0;
			for (int32_t value = 0; value <= kMacroFull; value += kMacroUnitsPerKnobStep) {
				most = std::max(most, encodeCommand(macro(Macro::Energy, value), 1, out));
			}
			expect(most).to_equal(5);
			expect(encodeCommand(macro(Macro::Energy, 25), 1, out)).to_equal(4);
		});

		it("takes a command at its widest in kMaxCommandBytes", _ {
			uint8_t out[kMaxCommandBytes];
			const Command widest{CommandKind::SetHome, 255, std::numeric_limits<int32_t>::min(), 65535, Lane::Riser};
			expect(encodeCommand(widest, std::numeric_limits<uint32_t>::max(), out)).to_equal(kMaxCommandBytes);
		});

		it("keeps a re-roll's path and a dropped swap beside the commands", _ {
			Buffer b(4096);
			b.log.beginPiece(startOf(1));
			b.log.append(64, {CommandKind::SetSound, 2, 9});
			b.log.appendSoundPath(64, 2, 9, pickSlot(2), "SYNTHS/GEN NEW");
			b.log.appendSoundDropped(65, 2);
			const std::vector<LogRecord> read = records(b.log, 0);
			expect(read.size()).to_equal(3u);
			expect(read[1].kind == RecordKind::SoundPath).to_be_true();
			expect(read[1].step).to_equal(64);
			expect(read[1].command.target).to_equal(2);
			expect(read[1].command.value).to_equal(9);
			expect(read[1].slot).to_equal(pickSlot(2));
			expect(std::string(read[1].path, static_cast<size_t>(read[1].pathLength))).to_equal("SYNTHS/GEN NEW");
			expect(read[2].kind == RecordKind::SoundDropped).to_be_true();
			expect(read[2].step).to_equal(65);
			expect(read[2].command.target).to_equal(2);
		});
	});

	context("pieces", _ {
		it("spans a piece to its end, or to where the log cut it if that came first; -1 while it plays uncut", _ {
			Buffer b(1024);
			b.log.beginPiece(startOf(1));
			b.log.append(0, macro(Macro::Energy, 1));
			expect(b.log.piece(0).spanEnd()).to_equal(-1);
			b.log.endPiece(40, PieceEnd::Stopped);
			expect(b.log.piece(0).spanEnd()).to_equal(40);

			Buffer full(1024);
			full.log.beginPiece(startOf(1));
			int32_t step = 0;
			while (full.log.append(step, macro(Macro::Energy, step % 100))) {
				step++;
			}
			expect(full.log.piece(0).spanEnd()).to_equal(step); // cut, still playing
			full.log.endPiece(step + 5, PieceEnd::Stopped);
			expect(full.log.piece(0).spanEnd()).to_equal(step); // the cut came first
		});

		it("opens each piece with its Recipe, seed, takes, picks and snapshot", _ {
			Buffer b(4096);
			PieceStart start = startOf(42);
			start.takes.take[1] = 3;
			start.first = SectionKind::Groove;
			start.onDrop = true;
			start.held = true;
			b.log.beginPiece(start);
			b.log.addPick(pickSlot(1), "SYNTHS/GEN ACID");
			b.log.addPick(pickSlot(Lane::Kick), "SAMPLES/KICK.WAV");
			b.log.addPick(pickSlot(2), "");
			const int32_t params[2] = {5, -5};
			b.log.addSnapshot(params, sizeof params);
			b.log.append(0, macro(Macro::Energy, 9));
			const PieceImage pieceImage = image(b.log, 0);
			const SteeringLog::Piece piece = pieceImage.piece();
			expect(piece.start().recipe == start.recipe).to_be_true();
			expect(piece.start().seed).to_equal(42u);
			expect(piece.start().takes == start.takes).to_be_true();
			expect(piece.start().first == SectionKind::Groove).to_be_true();
			expect(piece.start().onDrop).to_be_true();
			expect(piece.start().held).to_be_true();
			char path[kMaxSoundPath];
			expect(piece.pick(pickSlot(1), path)).to_be_true();
			expect(std::string(path)).to_equal("SYNTHS/GEN ACID");
			expect(piece.pick(pickSlot(Lane::Kick), path)).to_be_true();
			expect(std::string(path)).to_equal("SAMPLES/KICK.WAV");
			expect(piece.pick(pickSlot(2), path)).to_be_false();
			std::vector<int32_t> snapshot;
			piece.forEachSnapshot([&](const void* data, int32_t length) {
				snapshot.resize(static_cast<size_t>(length) / sizeof(int32_t));
				std::memcpy(snapshot.data(), data, static_cast<size_t>(length));
			});
			expect(snapshot == std::vector<int32_t>{5, -5}).to_be_true();
			expect(records(b.log, 0).size()).to_equal(1u);
			expect(piece.ended() == PieceEnd::Playing).to_be_true();
			expect(piece.endStep()).to_equal(-1);
		});

		it("keeps the last piece until the next begins, then throws it away unless marked", _ {
			Buffer b(4096);
			b.log.beginPiece(startOf(1));
			b.log.append(3, macro(Macro::Energy, 1));
			b.log.endPiece(10, PieceEnd::Stopped);
			expect(b.log.pieces()).to_equal(1);
			expect(b.log.piece(0).endStep()).to_equal(10);
			expect(b.log.piece(0).ended() == PieceEnd::Stopped).to_be_true();

			b.log.beginPiece(startOf(2));
			expect(b.log.pieces()).to_equal(1);
			expect(b.log.piece(0).start().seed).to_equal(2u);
			b.log.mark(true);
			b.log.append(5, macro(Macro::Energy, 2));
			b.log.endPiece(20, PieceEnd::Outro);
			b.store.drain(); // its header is on the card: the next piece opens

			b.log.beginPiece(startOf(3));
			expect(b.log.pieces()).to_equal(2);
			expect(b.log.piece(0).marked()).to_be_true();
			expect(b.log.piece(0).start().seed).to_equal(2u);
			expect(b.log.piece(0).ended() == PieceEnd::Outro).to_be_true();
			expect(b.log.piece(1).start().seed).to_equal(3u);
			expect(b.log.piece(1).marked()).to_be_false();
			// Unmarked, the current piece goes when the next begins, and its file with it.
			b.log.append(7, macro(Macro::Density, 4));
			b.log.endPiece(30, PieceEnd::Stopped);
			const uint32_t unmarked = b.log.piece(1).id();
			b.log.beginPiece(startOf(4));
			expect(b.log.pieces()).to_equal(2);
			expect(b.log.piece(1).start().seed).to_equal(4u);
			b.store.drain();
			expect(b.store.files.contains(unmarked)).to_be_false();
			expect(records(b.log, 0).size()).to_equal(1u);
			expect(records(b.log, 0)[0].step).to_equal(5);
		});

		it("cuts the piece at the last step that fit, dropping that step's records whole", _ {
			Buffer b(1024);
			b.log.beginPiece(startOf(1));
			int32_t step = 0;
			while (b.log.append(step, macro(Macro::Energy, step % 100))
			       && b.log.append(step, macro(Macro::Density, step % 100))) {
				step++;
			}
			expect(b.log.full()).to_be_true();
			expect(b.log.logging()).to_be_false();
			expect(b.log.piece(0).cutAt()).to_equal(step);
			const std::vector<LogRecord> read = records(b.log, 0);
			expect(static_cast<int32_t>(read.size())).to_equal(2 * step);
			expect(read.back().step).to_equal(step - 1);
			expect(b.log.append(step + 1, macro(Macro::Energy, 1))).to_be_false();
			b.log.endPiece(step + 5, PieceEnd::Stopped);
			expect(b.log.piece(0).endStep()).to_equal(step + 5);
			// The next piece has the room back.
			b.log.beginPiece(startOf(2));
			expect(b.log.full()).to_be_false();
			expect(b.log.append(0, macro(Macro::Energy, 1))).to_be_true();
		});

		it("says a swap was dropped even after the log was cut, inside the piece's span (§17.2)", _ {
			Buffer b(1024);
			b.log.beginPiece(startOf(1));
			b.log.append(3, {CommandKind::SetSound, 2, 9});
			int32_t step = 4;
			while (b.log.append(step, macro(Macro::Energy, step))) {
				step++;
			}
			const int32_t cutAt = b.log.piece(0).cutAt();
			expect(cutAt).to_equal(step);
			// The drop comes later than the cut, a few steps on: stamped on the span's last step.
			expect(b.log.appendSoundDropped(step + 3, 2)).to_be_true();
			const std::vector<LogRecord> read = records(b.log, 0);
			expect(read.back().kind == RecordKind::SoundDropped).to_be_true();
			expect(read.back().step).to_equal(cutAt - 1);
			expect(read.back().command.target).to_equal(2);
			bool ordered = true;
			for (size_t i = 1; i < read.size(); i++) {
				ordered = ordered && read[i].step >= read[i - 1].step;
			}
			expect(ordered).to_be_true();
		});

		it("writes no dropped record for a SetSound the cut took, and one for every part's landing however full the log",
		   _ {
			Buffer cutAway(1024);
			cutAway.log.beginPiece(startOf(1));
			int32_t step = 0;
			while (cutAway.log.append(step, macro(Macro::Energy, step))) {
				step++;
			}
			expect(cutAway.log.append(step, {CommandKind::SetSound, 2, 9})).to_be_false();
			expect(cutAway.log.appendSoundDropped(step + 1, 2)).to_be_false();
			expect(records(cutAway.log, 0).back().kind == RecordKind::Command).to_be_true();

			// Every part's landing still waiting for its drop gets one from the reserve, however full the log is: no new
			// SetSound lands once the records' room is gone, so at most one a part is waiting.
			Buffer b(1024);
			b.log.beginPiece(startOf(1));
			for (int32_t p = 0; p < kMaxParts; p++) {
				b.log.append(6, {CommandKind::SetSound, static_cast<uint8_t>(p), 4});
			}
			step = 7;
			while (b.log.append(step, macro(Macro::Energy, step))) {
				step++;
			}
			for (int32_t p = 0; p < kMaxParts; p++) {
				expect(b.log.appendSoundDropped(step + 1000, static_cast<uint8_t>(p))).to_be_true();
			}
			int32_t dropped = 0;
			for (const LogRecord& r : records(b.log, 0)) {
				dropped += r.kind == RecordKind::SoundDropped ? 1 : 0;
			}
			expect(dropped).to_equal(kMaxParts);
			expect(b.log.piece(0).cutAt()).to_equal(step);
		});

		it("stamps a dropped record no earlier than the record before it", _ {
			Buffer b(1024);
			b.log.beginPiece(startOf(1));
			b.log.append(10, {CommandKind::SetSound, 0, 3});
			b.log.append(12, macro(Macro::Energy, 1));
			expect(b.log.appendSoundDropped(11, 0)).to_be_true();
			const std::vector<LogRecord> read = records(b.log, 0);
			expect(read.back().kind == RecordKind::SoundDropped).to_be_true();
			expect(read.back().step).to_equal(12);
		});

		it("keeps the whole header when the piece's first record is the one that doesn't fit", _ {
			Buffer b(kDroppedReserve + 2); // 2 bytes for records: none fits
			b.log.beginPiece(startOf(1));
			b.log.addPick(pickSlot(1), "SYNTHS/GEN ACID");
			std::vector<uint8_t> snapshot(600);
			b.log.addSnapshot(snapshot.data(), static_cast<int32_t>(snapshot.size()));
			const int32_t header = b.log.piece(0).headerBytes();
			expect(b.log.full()).to_be_false();
			expect(b.log.append(5, macro(Macro::Energy, 1))).to_be_false();
			expect(b.log.piece(0).cutAt()).to_equal(5);
			expect(b.log.piece(0).headerBytes()).to_equal(header);
			expect(b.log.used()).to_equal(0); // no record: the piece holds its header alone
			char path[kMaxSoundPath];
			expect(image(b.log, 0).piece().pick(pickSlot(1), path)).to_be_true();
		});


		it("keeps a drop when a later record on its step is the one that doesn't fit", _ {
			Buffer b(1024);
			b.log.beginPiece(startOf(1));
			b.log.append(3, {CommandKind::SetSound, 2, 9});
			int32_t step = 4;
			// Fill to within a wide command of the end, then a drop and a command on one step.
			while (1024 - kDroppedReserve - b.log.used() > 2 * kMaxCommandBytes) {
				b.log.append(step++, macro(Macro::Energy, 1));
			}
			b.log.append(step, macro(Macro::Energy, 2));
			expect(b.log.appendSoundDropped(step, 2)).to_be_true();
			const Command widest{CommandKind::SetHome, 255, std::numeric_limits<int32_t>::min(), 65535, Lane::Riser};
			while (b.log.append(step, widest)) {}
			expect(b.log.piece(0).cutAt()).to_equal(step);
			const std::vector<LogRecord> read = records(b.log, 0);
			expect(read.back().kind == RecordKind::SoundDropped).to_be_true();
			expect(read.back().step).to_equal(step - 1);
			expect(read.back().command.target).to_equal(2);
			expect(read.front().command.kind == CommandKind::SetSound).to_be_true();
		});

		it("writes no drop for a SetSound the cut refused, and none for the landing before it, which was heard", _ {
			for (const bool triggersTheCut : {false, true}) {
				Buffer b(1024);
				b.log.beginPiece(startOf(1));
				b.log.append(3, {CommandKind::SetSound, 2, 9}); // lands and plays
				int32_t step = 4;
				while (1024 - kDroppedReserve - b.log.used() >= 4) { // a macro of 1 a step on takes 4 bytes
					b.log.append(step++, macro(Macro::Energy, 1));
				}
				expect(b.log.full()).to_be_false();
				const Command widest{CommandKind::SetHome, 255, std::numeric_limits<int32_t>::min(), 65535, Lane::Riser};
				if (!triggersTheCut) {
					while (b.log.append(step, widest)) {} // cut by another record first
					step += 10;
				}
				// The next SetSound on part 2 is refused, and a confirm drops that swap.
				expect(b.log.append(step, {CommandKind::SetSound, 2, 11})).to_be_false();
				expect(b.log.full()).to_be_true();
				expect(b.log.appendSoundDropped(step + 1, 2)).to_be_false();
				bool dropped = false;
				for (const LogRecord& r : records(b.log, 0)) {
					dropped = dropped || r.kind == RecordKind::SoundDropped;
				}
				expect(dropped).to_be_false();
			}
		});

		it("writes one drop per landing", _ {
			Buffer b(1024);
			b.log.beginPiece(startOf(1));
			b.log.append(3, {CommandKind::SetSound, 2, 9});
			expect(b.log.appendSoundDropped(4, 2)).to_be_true();
			expect(b.log.appendSoundDropped(5, 2)).to_be_false();
			b.log.append(8, {CommandKind::SetSound, 2, 10});
			expect(b.log.appendSoundDropped(9, 2)).to_be_true();
		});

		it("cuts a piece whose header doesn't fit at its step 0", _ {
			Buffer b(1024, 256);
			b.log.beginPiece(startOf(1));
			std::vector<uint8_t> snapshot(300);
			b.log.addPick(pickSlot(1), "SYNTHS/GEN ACID");
			b.log.addSnapshot(snapshot.data(), static_cast<int32_t>(snapshot.size()));
			expect(b.log.full()).to_be_true();
			expect(b.log.piece(0).cutAt()).to_equal(0);
			// None of its header stays: a pick without its snapshot can't be built.
			char path[kMaxSoundPath];
			expect(image(b.log, 0).piece().pick(pickSlot(1), path)).to_be_false();
			expect(b.log.append(0, macro(Macro::Energy, 1))).to_be_false();
		});

		it("opens no piece while the kept piece's header is still on its way to the card", _ {
			Buffer b(1024);
			b.log.beginPiece(startOf(1));
			b.log.mark(true);
			int32_t step = 0;
			while (b.log.append(step, macro(Macro::Energy, 1))) {
				step++;
			}
			b.log.endPiece(step, PieceEnd::Stopped);
			b.log.beginPiece(startOf(2));
			expect(b.log.pieces()).to_equal(1);
			expect(b.log.full()).to_be_true();
			expect(b.log.logging()).to_be_false();
			expect(b.log.piece(0).start().seed).to_equal(1u);
		});

	});

	context("marks (§17.4)", _ {
		it("marks and unmarks the current piece, playing or the last one after a stop", _ {
			Buffer b(4096);
			expect(b.log.current()).to_equal(-1);
			expect(b.log.mark(true)).to_be_false(); // no piece yet
			b.log.beginPiece(startOf(1));
			expect(b.log.current()).to_equal(0);
			expect(b.log.mark(true)).to_be_true();
			expect(b.log.piece(0).marked()).to_be_true();
			expect(b.log.markedPieces()).to_equal(1);
			expect(b.log.mark(false)).to_be_true();
			expect(b.log.piece(0).marked()).to_be_false();
			b.log.endPiece(10, PieceEnd::Stopped);
			expect(b.log.current()).to_equal(0); // the last piece, which the leave menu offers
			expect(b.log.mark(true)).to_be_true();
			b.store.drain();
			b.log.beginPiece(startOf(2));
			expect(b.log.pieces()).to_equal(2);
			expect(b.log.current()).to_equal(1);
			expect(b.log.piece(1).marked()).to_be_false();
			expect(b.log.markedPieces()).to_equal(1);
		});

		it("refuses a mark once the log is full, but still takes an unmark", _ {
			Buffer b(1024);
			b.log.beginPiece(startOf(1));
			int32_t step = 0;
			while (b.log.append(step, macro(Macro::Energy, 1))) {
				step++;
			}
			expect(b.log.full()).to_be_true();
			expect(b.log.mark(true)).to_be_false(); // not marked when the log cut it
			expect(b.log.piece(0).marked()).to_be_false();
			expect(b.log.mark(false)).to_be_true();
			expect(b.log.piece(0).marked()).to_be_false();
		});

		it("has no current piece when the next one found no room to open, so a press can't touch the marked ones", _ {
			Buffer b(1024);
			b.log.beginPiece(startOf(1));
			b.log.mark(true);
			int32_t step = 0;
			while (b.log.append(step, macro(Macro::Energy, 1))) {
				step++;
			}
			b.log.endPiece(step, PieceEnd::Stopped);
			b.log.beginPiece(startOf(2));
			expect(b.log.current()).to_equal(-1);
			expect(b.log.mark(false)).to_be_false();
			expect(b.log.piece(0).marked()).to_be_true();
		});

		it("drops an earlier unmarked piece at the next piece, and marks it again until then", _ {
			Buffer b(8192);
			threeMarked(b.log);
			// The first piece, unmarked from the CAPTURE menu: it stays until the next piece begins, and can be
			// marked again until then, even with the log full.
			expect(b.log.markPiece(0, false)).to_be_true();
			expect(b.log.markedPieces()).to_equal(2);
			expect(b.log.markPiece(0, true)).to_be_true();
			expect(b.log.markPiece(0, false)).to_be_true();
			expect(b.log.pieces()).to_equal(3);

			const uint32_t opened = b.log.opened();
			b.store.drain();
			b.log.beginPiece(startOf(4));
			expect(b.log.opened()).to_equal(opened + 1);
			expect(b.log.pieces()).to_equal(3);
			expect(seeds(b.log)).to_equal(std::vector<uint32_t>{2, 3, 4});
			expect(b.log.current()).to_equal(2);
			expect(b.log.markedPieces()).to_equal(2);
		});

		it("takes marks again after an unmarked earlier piece made room at the next piece", _ {
			Buffer b(1024);
			b.log.beginPiece(startOf(1));
			b.log.mark(true);
			int32_t step = 0;
			while (step < 60 && b.log.append(step, macro(Macro::Energy, 1))) {
				step++;
			}
			b.log.endPiece(step, PieceEnd::Stopped);
			b.log.beginPiece(startOf(2));
			while (b.log.append(step, macro(Macro::Energy, 1))) {
				step++;
			}
			expect(b.log.full()).to_be_true();
			expect(b.log.markPiece(0, false)).to_be_true();
			expect(b.log.mark(true)).to_be_false(); // the current piece stays cut until it ends
			b.log.endPiece(step, PieceEnd::Stopped);
			b.log.beginPiece(startOf(3));
			expect(b.log.pieces()).to_equal(1);
			expect(b.log.full()).to_be_false();
			expect(b.log.mark(true)).to_be_true();
		});

		it("puts back a mark the cut found: a second RECORD on a full log doesn't lose a piece that was marked", _ {
			Buffer b(1024);
			b.log.beginPiece(startOf(1));
			b.log.mark(true);
			int32_t step = 0;
			while (b.log.append(step, macro(Macro::Energy, 1))) {
				step++;
			}
			expect(b.log.full()).to_be_true();
			expect(b.log.mark(false)).to_be_true();
			expect(b.log.mark(true)).to_be_true();
			expect(b.log.piece(0).marked()).to_be_true();
		});
	});

	context("on the card (§17.2)", _ {
		it("hands the store each piece's opening, header and records in order, the header once whole, a step once past", _ {
			Buffer b(1024);
			b.log.beginPiece(startOf(1));
			b.log.addPick(pickSlot(1), "SYNTHS/GEN ACID");
			LogChunk chunk;
			expect(b.log.nextChunk(chunk, 4096)).to_be_false(); // the header may still grow
			b.log.commitBefore(0); // step 0's decision: the header is whole
			expect(b.log.nextChunk(chunk, 4096)).to_be_true();
			expect(chunk.offset).to_equal(0);
			expect(chunk.length).to_equal(static_cast<int32_t>(sizeof(PieceInfo))); // the opening alone
			b.log.took(chunk);
			expect(b.log.nextChunk(chunk, 4096)).to_be_true();
			expect(chunk.offset).to_equal(static_cast<int32_t>(sizeof(PieceInfo)));
			b.log.took(chunk);
			expect(b.log.nextChunk(chunk, 4096)).to_be_false();
			b.log.append(0, macro(Macro::Energy, 1));
			b.log.append(3, macro(Macro::Energy, 2));
			b.log.commitBefore(3); // step 3 is being decided: only step 0's record is final
			expect(b.log.nextChunk(chunk, 4096)).to_be_true();
			expect(chunk.offset).to_equal(b.log.piece(0).headerBytes());
			const int32_t first = chunk.length;
			b.log.took(chunk);
			expect(b.log.nextChunk(chunk, 4096)).to_be_false();
			b.log.endPiece(5, PieceEnd::Stopped); // everything is final
			expect(b.log.nextChunk(chunk, 4096)).to_be_true();
			expect(chunk.offset).to_equal(b.log.piece(0).headerBytes() + first);
			b.log.took(chunk);
			expect(b.log.allTaken(b.log.slotOf(0))).to_be_true();
			expect(b.log.used()).to_equal(0);
		});

		it("rides out a card stall in RAM, and cuts the piece as a full log only when the ring fills (spec §17.2)", _ {
			Buffer b(1024);
			b.log.beginPiece(startOf(1));
			int32_t step = 0;
			for (; step < 100; step++) {
				b.log.commitBefore(step);
				expect(b.log.append(step, macro(Macro::Energy, step % 50))).to_be_true();
			}
			b.log.commitBefore(step);
			b.store.drain(); // the card is back: the ring empties
			expect(b.log.used()).to_equal(0);
			for (; b.log.append(step, macro(Macro::Energy, step % 50)); step++) {
				b.log.commitBefore(step + 1);
			}
			expect(b.log.full()).to_be_true();
			expect(b.log.piece(0).cutAt()).to_equal(step);
			b.log.endPiece(step + 10, PieceEnd::Stopped);
			const std::vector<LogRecord> read = records(b.log, 0);
			expect(static_cast<int32_t>(read.size())).to_equal(step);
			expect(read.back().step).to_equal(step - 1);
			// The next piece has the ring back once the card took the last one.
			b.store.drain();
			b.log.beginPiece(startOf(2));
			expect(b.log.full()).to_be_false();
			expect(b.log.append(0, macro(Macro::Energy, 1))).to_be_true();
		});

		it("cuts the playing piece when the card fails, and the file reads back up to its last whole step", _ {
			Buffer b(4096);
			b.log.beginPiece(startOf(1));
			b.log.addPick(pickSlot(1), "SYNTHS/GEN ACID");
			b.log.mark(true);
			int32_t step = 0;
			for (; step < 40; step++) {
				b.log.commitBefore(step);
				b.log.append(step, macro(Macro::Energy, step));
				b.log.append(step, macro(Macro::Density, step));
			}
			b.store.drain();
			const int32_t stored = static_cast<int32_t>(b.store.files[b.log.piece(0).id()].size());
			b.store.failFrom = stored + 7; // the card fills partway through the next write
			for (; step < 80 && b.log.logging(); step++) {
				b.log.commitBefore(step);
				b.log.append(step, macro(Macro::Energy, step));
				b.log.append(step, macro(Macro::Density, step));
				b.store.drain();
			}
			expect(b.log.full()).to_be_true();
			expect(b.log.storeFailing()).to_be_true();
			expect(b.log.mark(true)).to_be_true(); // it was marked when the card cut it
			b.log.endPiece(step, PieceEnd::Stopped);
			const PieceImage file = b.store.stored(0);
			expect(file.bytes.empty()).to_be_false();
			const SteeringLog::Piece piece = file.piece();
			// What the last flush stored, steps 0 to 38 (step 39 was being decided): none of the steps after it.
			expect(piece.cutAt()).to_equal(39);
			expect(piece.spanEnd()).to_equal(39);
			std::vector<LogRecord> read;
			piece.forEachRecord([&](const LogRecord& r) { read.push_back(r); });
			expect(static_cast<int32_t>(read.size())).to_equal(78);
			expect(read.back().step).to_equal(38);
			char path[kMaxSoundPath];
			expect(piece.pick(pickSlot(1), path)).to_be_true();
		});

		it("reads a piece cut short by a power-off back to its last whole step, or the last step a quiet stretch reached", _ {
			Buffer b(4096);
			b.log.beginPiece(startOf(1));
			b.log.mark(true);
			for (int32_t step = 0; step < 200; step++) {
				b.log.commitBefore(step);
				if (step < 30 && step % 3 == 0) {
					b.log.append(step, macro(Macro::Energy, step));
				}
			}
			b.store.drain(); // the last flush before the power went
			std::vector<uint8_t> file = b.store.files[b.log.piece(0).id()];
			// Its info never said it ended; the steps after the last steering said how far it got.
			expect(SteeringLog::readImage(file.data(), static_cast<int32_t>(file.size()))).to_be_true();
			const SteeringLog::Piece quiet(file.data());
			expect(quiet.ended() == PieceEnd::Stopped).to_be_true();
			expect(quiet.spanEnd()).to_be_greater_than(198 - SteeringLog::kReachedEvery - 1);
			expect(quiet.spanEnd()).to_be_less_than(200);
			expect(quiet.recordCount()).to_equal(10);
			// A record the card has only part of goes, with the rest of its step.
			Buffer c(4096);
			c.log.beginPiece(startOf(2));
			for (int32_t step = 0; step < 6; step++) {
				c.log.commitBefore(step);
				c.log.append(step, macro(Macro::Energy, step));
			}
			c.log.commitBefore(6);
			c.store.drain();
			std::vector<uint8_t> torn = c.store.files[c.log.piece(0).id()];
			torn.pop_back();
			expect(SteeringLog::readImage(torn.data(), static_cast<int32_t>(torn.size()))).to_be_true();
			const SteeringLog::Piece cut(torn.data());
			expect(cut.cutAt()).to_equal(5);
			expect(cut.recordCount()).to_equal(5);
			// A header the card has only part of leaves the opening alone, cut at step 0.
			std::vector<uint8_t> headerless(b.store.files[b.log.piece(0).id()].begin(),
			                                b.store.files[b.log.piece(0).id()].begin() + sizeof(PieceInfo));
			PieceInfo info;
			std::memcpy(&info, headerless.data(), sizeof info);
			info.headerBytes = sizeof(PieceInfo) + 40;
			std::memcpy(headerless.data(), &info, sizeof info);
			expect(SteeringLog::readImage(headerless.data(), static_cast<int32_t>(headerless.size()))).to_be_true();
			expect(SteeringLog::Piece(headerless.data()).cutAt()).to_equal(0);
		});

		it("refuses an image of another format, or of a genre version this firmware doesn't have", _ {
			Buffer b(4096);
			b.log.beginPiece(startOf(1));
			b.log.endPiece(4, PieceEnd::Stopped);
			b.store.drain();
			const std::vector<uint8_t> good = b.store.files[b.log.piece(0).id()];
			std::vector<uint8_t> file = good;
			expect(SteeringLog::readImage(file.data(), static_cast<int32_t>(file.size()))).to_be_true();
			expect(SteeringLog::Piece(file.data()).start().recipe.genre == acidRecipe().genre).to_be_true();
			for (const int32_t field : {0, 1, 2}) {
				file = good;
				PieceInfo info;
				std::memcpy(&info, file.data(), sizeof info);
				if (field == 0) {
					info.magic ^= 1;
				}
				else if (field == 1) {
					info.format++;
				}
				else {
					info.genreVersion++;
				}
				std::memcpy(file.data(), &info, sizeof info);
				expect(SteeringLog::readImage(file.data(), static_cast<int32_t>(file.size()))).to_be_false();
			}
		});

		it("takes marked pieces found on the card back as earlier marked pieces, its own ids going on past theirs", _ {
			Buffer b(4096);
			PieceInfo found;
			found.id = 41;
			found.bytes = sizeof(PieceInfo);
			found.headerBytes = sizeof(PieceInfo);
			found.endStep = 64;
			found.ended = PieceEnd::Stopped;
			found.start = startOf(9);
			expect(b.log.adopt(found)).to_be_true();
			expect(b.log.nextId()).to_equal(42u);
			expect(b.log.current()).to_equal(-1); // not the current piece: RECORD can't unmark it
			b.log.beginPiece(startOf(1));
			expect(b.log.pieces()).to_equal(2);
			expect(b.log.piece(0).marked()).to_be_true();
			expect(b.log.piece(0).start().seed).to_equal(9u);
			expect(b.log.piece(1).id()).to_equal(42u);
			b.store.drain();
			expect(b.store.files.contains(41)).to_be_false(); // on the card already: nothing is written again
		});

		it("drops a thrown-away piece's bytes still in RAM and gives its room back", _ {
			Buffer b(1024);
			b.log.beginPiece(startOf(1));
			for (int32_t step = 0; step < 30; step++) {
				b.log.append(step, macro(Macro::Energy, step));
			}
			b.log.endPiece(30, PieceEnd::Stopped);
			const uint32_t first = b.log.piece(0).id();
			b.log.beginPiece(startOf(2)); // unmarked, the first piece goes, its records never stored
			b.store.drain();
			expect(b.store.files.contains(first)).to_be_false();
			expect(b.log.used()).to_equal(0);
		});

		it("opens no piece when the table is full of marked pieces", _ {
			Buffer b(4096);
			for (int32_t i = 0; i < kMaxLogPieces; i++) {
				b.store.drain();
				b.log.beginPiece(startOf(static_cast<uint32_t>(i)));
				b.log.mark(true);
				b.log.endPiece(4, PieceEnd::Stopped);
			}
			b.store.drain();
			b.log.beginPiece(startOf(99));
			expect(b.log.full()).to_be_true();
			expect(b.log.current()).to_equal(-1);
			expect(b.log.pieces()).to_equal(kMaxLogPieces);
		});
	});

	context("Perform's pieces", _ {
		it("logs a param edit on the next undecided step, one value per param a step", _ {
			StepwisePiece<HeaderSink> piece{{.seed = 3, .skipIntro = true, .logEntries = 64}};
			piece.decideBefore(4);
			piece.push({CommandKind::SetParam, 1, 10, 0x2005, Lane::None});
			piece.push({CommandKind::SetParam, kSongParams, 30, 7, Lane::None});
			piece.push({CommandKind::SetParam, 1, 20, 0x2005, Lane::None}); // the same param: its latest value
			piece.push({CommandKind::SetParam, 0, 40, 7, Lane::Kick});      // a Kit row's
			piece.push({CommandKind::SetParam, kMaxParts, 1, 7, Lane::None}); // no such part
			piece.decideBefore(6);
			const std::vector<LogRecord> read = records(piece.log, 0);
			expect(read.size()).to_equal(3u);
			expect(read[0].step).to_equal(4);
			expect(read[0].command == Command{CommandKind::SetParam, 1, 20, 0x2005, Lane::None}).to_be_true();
			expect(read[1].command == Command{CommandKind::SetParam, kSongParams, 30, 7, Lane::None}).to_be_true();
			expect(read[2].command == Command{CommandKind::SetParam, 0, 40, 7, Lane::Kick}).to_be_true();
			expect(read[2].step).to_equal(4);
		});

		it("opens a piece at a start with the Recipe, seed and takes, and ends it at a stop", _ {
			StepwisePiece<HeaderSink> piece{{.seed = 3, .skipIntro = true, .logEntries = 64}};
			piece.decideBefore(5);
			piece.push(macro(Macro::Energy, 7));
			piece.decideBefore(10);
			piece.driver.stop(piece.sink);
			const SteeringLog& log = piece.log;
			expect(log.pieces()).to_equal(1);
			const PieceImage pImage = image(log, 0);
			const SteeringLog::Piece p = pImage.piece();
			expect(p.start().seed).to_equal(3u);
			expect(p.start().recipe.macros[Macro::Energy]).to_equal(acidRecipe().macros[Macro::Energy]);
			expect(p.start().first == SectionKind::Groove).to_be_true();
			expect(p.start().onDrop).to_be_false();
			expect(p.ended() == PieceEnd::Stopped).to_be_true();
			expect(p.endStep()).to_equal(10);
			expect(piece.sink.headers).to_equal(1);
			char path[kMaxSoundPath];
			expect(p.pick(pickSlot(1), path)).to_be_true();
			const std::vector<LogRecord> read = records(log, 0);
			expect(read.size()).to_equal(1u);
			expect(read[0].step).to_equal(5);
		});

		it("logs a Part bias where it lands, and the next piece's header carries the bias (#303)", _ {
			Recipe biased = acidRecipe();
			biased.bias.set(1, Macro::Density, -640);
			StepwisePiece<HeaderSink> piece{{.recipe = biased, .seed = 3, .skipIntro = true, .logEntries = 64}};
			piece.decideBefore(4);
			const Command bias{CommandKind::SetBias, 1, 960, static_cast<uint16_t>(Macro::Energy)};
			piece.push(bias);
			piece.decideBefore(6);
			const std::vector<LogRecord> read = records(piece.log, 0);
			expect(read.size()).to_equal(1u);
			expect(read[0].step).to_equal(4);
			expect(read[0].command == bias).to_be_true();
			const PieceStart& start = piece.log.piece(0).start();
			expect(start.recipe.bias.of(1, Macro::Density)).to_equal(-640);
			expect(start.recipe.bias.of(1, Macro::Energy)).to_equal(0); // the header is step 0's, before it landed
		});

		it("starts a new piece at the next start, throwing the unmarked one away", _ {
			StepwisePiece<HeaderSink> piece{{.seed = 3, .skipIntro = true, .logEntries = 64}};
			piece.decideBefore(4);
			piece.push(macro(Macro::Energy, 7));
			piece.decideBefore(8);
			piece.restart(Start{.seed = 5, .logEntries = 64});
			piece.decideBefore(2);
			expect(piece.log.pieces()).to_equal(1);
			expect(piece.log.piece(0).start().seed).to_equal(5u);
			expect(piece.log.piece(0).start().first == SectionKind::Intro).to_be_true();
		});

		it("ends the piece on a restart's line, and the restarted run opens a piece of its own (#413)", _ {
			StepwisePiece<HeaderSink> piece{{.seed = 3, .skipIntro = true, .logEntries = 64}};
			piece.decideBefore(4);
			piece.log.mark(true); // kept, so both pieces read back
			piece.push(macro(Macro::Energy, 7));
			piece.steering.setMacro(Macro::Density, 31); // the Current Recipe's: the next header holds it
			piece.push({CommandKind::Restart, 0, 1});
			piece.runToStepLine(kStepsPerBar + 2);
			piece.push(macro(Macro::Density, 9));
			piece.runToStepLine(kStepsPerBar + 6);
			expect(piece.log.pieces()).to_equal(2);
			if (piece.log.pieces() != 2) {
				return; // piece(1) isn't there to read
			}
			const PieceImage pImage = image(piece.log, 0);
			const SteeringLog::Piece p = pImage.piece();
			expect(p.ended() == PieceEnd::Stopped).to_be_true();
			expect(p.endStep()).to_equal(kStepsPerBar);
			const std::vector<LogRecord> read = records(piece.log, 0);
			expect(read.size()).to_equal(2u);
			expect(read[0].step).to_equal(4);
			// The restarted run: its own header (the Current Recipe at its step 0, from the Intro) and its steering.
			const PieceImage restartedImage = image(piece.log, 1);
			const SteeringLog::Piece restarted = restartedImage.piece();
			expect(restarted.ended() == PieceEnd::Playing).to_be_true();
			expect(restarted.start().recipe.macros[Macro::Density]).to_equal(31);
			expect(restarted.start().first == SectionKind::Intro).to_be_true();
			expect(piece.sink.headers).to_equal(2);
			const std::vector<LogRecord> again = records(piece.log, 1);
			expect(again.size()).to_equal(1u);
			expect(again[0].command == macro(Macro::Density, 9)).to_be_true();
		});

		it("ends at the Outro's end", _ {
			TestLog entries(8);
			Run run;
			run.recipe = acidRecipe();
			run.seed = 3;
			run.skipIntro = true;
			run.log = &entries.log;
			run.steer = steerAt({{40, {CommandKind::End, 0, 1}}});
			Result result = play(run, 128);
			expect(result.ended).to_be_true();
			const PieceImage pImage = image(entries.log, entries.log.pieces() - 1);
			const SteeringLog::Piece p = pImage.piece();
			expect(p.ended() == PieceEnd::Outro).to_be_true();
			// The driver stops on the call that reaches the end line.
			expect(p.endStep() * kTicksPerStep <= result.endedAt).to_be_true();
			expect(p.endStep() * kTicksPerStep > result.endedAt - kTicksPerStep).to_be_true();
		});

		it("logs a re-roll's new picks on the step its sound lands", _ {
			StepwisePiece<HeaderSink> piece{{.seed = 3, .skipIntro = true, .logEntries = 64}};
			const int32_t phrase = piece.phraseSteps();
			piece.decideBefore(3);
			piece.push({CommandKind::SetSound, 1, 9});
			piece.decideBefore(phrase + 2);
			const std::vector<LogRecord> read = records(piece.log, 0);
			expect(read.size()).to_equal(2u);
			expect(read[0].kind == RecordKind::Command).to_be_true();
			expect(read[0].step).to_equal(phrase);
			expect(read[1].kind == RecordKind::SoundPath).to_be_true();
			expect(read[1].step).to_equal(phrase);
			expect(read[1].command.target).to_equal(1);
			expect(read[1].command.value).to_equal(9);
		});

		it("ends the outgoing piece at a transition's start step and opens the new one there, with what it carries over",
		   _ {
			TransitionPiece t;
			TestLog entries(256);
			t.driver.setLog(&entries.log);
			t.decideBefore(2);
			t.push({CommandKind::SetHold, 0, kPress});
			t.push(macro(Macro::Energy, 11));
			t.confirmAt(3, TransitionStyle::BuildIntoDrop);
			const int32_t phrase = t.phraseSteps();
			t.runBars(4 * phrase / kStepsPerBar);
			expect(t.sink.starts).to_equal(1);
			const SteeringLog& log = entries.log;
			// The outgoing piece was unmarked: once the new one opened, it went. Mark it this time.
			expect(log.pieces()).to_equal(1);
			const PieceImage incomingImage = image(log, 0);
			const SteeringLog::Piece incoming = incomingImage.piece();
			expect(incoming.start().onDrop).to_be_true();
			expect(incoming.start().first == SectionKind::Groove).to_be_true();
			expect(incoming.start().held).to_be_true();
			expect(incoming.start().recipe.macros[Macro::Energy]).to_equal(11);
			expect(incoming.ended() == PieceEnd::Playing).to_be_true();
		});

		it("logs the tempo glide into a transition's new Recipe, ending on the start step (§17.2 Glides)", _ {
			Recipe playing = acidRecipe();
			Recipe pending = acidRecipe();
			pending.tempo = playing.tempo + 1500;
			TransitionPiece t(playing, pending);
			TestLog entries(256);
			t.driver.setLog(&entries.log);
			t.decideBefore(2);
			entries.log.mark(true);
			t.confirmAt(3, TransitionStyle::BuildIntoDrop);
			t.runBars(4 * t.phraseSteps() / kStepsPerBar);
			expect(t.sink.starts).to_equal(1);
			std::vector<LogRecord> glides;
			image(entries.log, 0).piece().forEachRecord([&](const LogRecord& r) {
				if (r.kind == RecordKind::Command && r.command.kind == CommandKind::TransitionTempo) {
					glides.push_back(r);
				}
			});
			expect(glides.size()).to_equal(1u);
			if (glides.size() != 1u) {
				return;
			}
			expect(glides[0].command.value).to_equal(pending.tempo);
			expect(stepTick(glides[0].step) + glides[0].command.length).to_equal(t.sink.startAt);
			// Applied, it changes nothing in the core: the outgoing Recipe's tempo stays the one that played.
			StepwisePiece<> replay{{.recipe = playing, .seed = 3, .skipIntro = true}};
			replay.decideBefore(2);
			replay.push(glides[0].command);
			replay.decideBefore(4);
			expect(replay.generator.recipe().tempo).to_equal(playing.tempo);
		});

		it("logs the glide back when a cut is cancelled during its glide", _ {
			Recipe playing = acidRecipe();
			Recipe pending = acidRecipe();
			pending.tempo = playing.tempo - 2000;
			TransitionPiece t(playing, pending);
			TestLog entries(256);
			t.driver.setLog(&entries.log);
			const int32_t phrase = t.phraseSteps();
			t.confirmAt(3, TransitionStyle::Cut);
			t.decideBefore(phrase - 4); // a bar's glide ends on the cut line: it has started
			expect(t.lifecycle.cancel()).to_be_true();
			t.runBars(2);
			std::vector<LogRecord> glides;
			image(entries.log, entries.log.pieces() - 1).piece().forEachRecord([&](const LogRecord& r) {
				if (r.kind == RecordKind::Command && r.command.kind == CommandKind::TransitionTempo) {
					glides.push_back(r);
				}
			});
			expect(glides.size()).to_equal(2u);
			if (glides.size() != 2u) {
				return;
			}
			expect(glides[0].command.value).to_equal(pending.tempo);
			expect(glides[1].command.value).to_equal(playing.tempo);
			expect(glides[1].step >= glides[0].step).to_be_true();
		});

		it("keeps a marked outgoing piece, ended on its start step, with its Transition logged where it landed", _ {
			TransitionPiece t;
			TestLog entries(256);
			t.driver.setLog(&entries.log);
			t.decideBefore(2);
			entries.log.mark(true);
			t.confirmAt(3, TransitionStyle::BuildIntoDrop);
			const int32_t phrase = t.phraseSteps();
			t.runBars(4 * phrase / kStepsPerBar);
			const SteeringLog& log = entries.log;
			expect(log.pieces()).to_equal(2);
			const PieceImage outgoingImage = image(log, 0);
			const SteeringLog::Piece outgoing = outgoingImage.piece();
			expect(outgoing.ended() == PieceEnd::Transition).to_be_true();
			expect(outgoing.endStep() * kTicksPerStep).to_equal(t.sink.startAt);
			bool transition = false;
			outgoing.forEachRecord([&](const LogRecord& r) {
				transition = transition || (r.kind == RecordKind::Command && r.command.kind == CommandKind::Transition);
			});
			expect(transition).to_be_true();
			// The new piece counts its own steps from the start step.
			expect(log.piece(1).ended() == PieceEnd::Playing).to_be_true();
		});
	});
});

CPPSPEC_SPEC(steering_log)
