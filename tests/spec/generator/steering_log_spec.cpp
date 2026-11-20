// The steering log (spec §17.2, §17.3): its records, piece headers and pieces, in the buffer and as the Perform
// driver writes them.

#include "generative/core/steering_log.h"
#include "harness.h"
#include "stream_helpers.h"
#include "transition_piece.h"

#include "cppspec.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace harness;

namespace {

/// A buffer of `bytes` and its log, with nothing in it.
struct Buffer {
	explicit Buffer(int32_t size) : bytes(static_cast<size_t>(size)), log(bytes.data(), size) {}
	std::vector<uint8_t> bytes;
	SteeringLog log;
};

std::vector<LogRecord> records(const SteeringLog& log, int32_t piece) {
	std::vector<LogRecord> out;
	log.piece(piece).forEachRecord([&](const LogRecord& record) { out.push_back(record); });
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

/// Every kind, with values at the edges of what each field holds.
std::vector<Command> everyCommand() {
	std::vector<Command> out;
	for (int32_t k = 0; k <= static_cast<int32_t>(kLastCommandKind); k++) {
		const auto kind = static_cast<CommandKind>(k);
		for (int32_t value : {0, 1, -1, 63, -64, 64, 300, -300, std::numeric_limits<int32_t>::max(),
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
};

} // namespace

// clang-format off
describe steering_log("The steering log", $ {
	describe("records", _ {
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

	describe("pieces", _ {
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
			const SteeringLog::Piece piece = b.log.piece(0);
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
			const int32_t oneUsed = b.log.used();

			b.log.beginPiece(startOf(2));
			expect(b.log.pieces()).to_equal(1);
			expect(b.log.piece(0).start().seed).to_equal(2u);
			b.log.mark(true);
			b.log.append(5, macro(Macro::Energy, 2));
			b.log.endPiece(20, PieceEnd::Outro);

			b.log.beginPiece(startOf(3));
			expect(b.log.pieces()).to_equal(2);
			expect(b.log.piece(0).marked()).to_be_true();
			expect(b.log.piece(0).start().seed).to_equal(2u);
			expect(b.log.piece(0).ended() == PieceEnd::Outro).to_be_true();
			expect(b.log.piece(1).start().seed).to_equal(3u);
			expect(b.log.piece(1).marked()).to_be_false();
			expect(b.log.used()).to_be_greater_than(oneUsed);
			// Unmarked, the current piece goes when the next begins, and its room with it.
			b.log.append(7, macro(Macro::Density, 4));
			b.log.endPiece(30, PieceEnd::Stopped);
			const int32_t twoUsed = b.log.used();
			b.log.beginPiece(startOf(4));
			expect(b.log.pieces()).to_equal(2);
			expect(b.log.piece(1).start().seed).to_equal(4u);
			expect(b.log.used()).to_be_less_than(twoUsed);
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

		it("cuts a piece whose header doesn't fit at its step 0", _ {
			Buffer b(256);
			b.log.beginPiece(startOf(1));
			std::vector<uint8_t> snapshot(300);
			b.log.addSnapshot(snapshot.data(), static_cast<int32_t>(snapshot.size()));
			expect(b.log.full()).to_be_true();
			expect(b.log.piece(0).cutAt()).to_equal(0);
			expect(b.log.append(0, macro(Macro::Energy, 1))).to_be_false();
		});

		it("opens no piece when even its opening doesn't fit behind the marked ones", _ {
			Buffer b(1024);
			b.log.beginPiece(startOf(1));
			b.log.mark(true);
			while (b.log.append(0, macro(Macro::Energy, 1))) {}
			b.log.endPiece(1, PieceEnd::Stopped);
			b.log.beginPiece(startOf(2));
			expect(b.log.pieces()).to_equal(1);
			expect(b.log.full()).to_be_true();
			expect(b.log.logging()).to_be_false();
			expect(b.log.piece(0).start().seed).to_equal(1u);
		});

		it("holds 10 marked 5-minute pieces of busy steering in 128 KB (H13's sum, spec §20)", _ {
			// Busy steering: a macro turned a knob step on every other step of a 5-minute piece at 128 BPM (2,560
			// steps), sweeping its whole range, and the largest roster's 6 KB header.
			Buffer b(128 * 1024);
			std::vector<uint8_t> header(6 * 1024);
			int32_t kept = 0;
			for (int32_t piece = 0; piece < 20; piece++) {
				b.log.beginPiece(startOf(static_cast<uint32_t>(piece)));
				b.log.addSnapshot(header.data(), static_cast<int32_t>(header.size()));
				for (int32_t step = 0; step < 2560; step += 2) {
					b.log.append(step, macro(Macro::Energy, (step / 2 * kMacroUnitsPerKnobStep) % kMacroFull));
				}
				if (b.log.full()) {
					break;
				}
				b.log.mark(true);
				b.log.endPiece(2560, PieceEnd::Stopped);
				kept++;
			}
			expect(kept).to_equal(10);
		});
	});

	describe("Perform's pieces", _ {
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
			const SteeringLog::Piece p = log.piece(0);
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
			const SteeringLog::Piece p = entries.log.piece(entries.log.pieces() - 1);
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
			const SteeringLog::Piece incoming = log.piece(0);
			expect(incoming.start().onDrop).to_be_true();
			expect(incoming.start().first == SectionKind::Groove).to_be_true();
			expect(incoming.start().held).to_be_true();
			expect(incoming.start().recipe.macros[Macro::Energy]).to_equal(11);
			expect(incoming.ended() == PieceEnd::Playing).to_be_true();
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
			const SteeringLog::Piece outgoing = log.piece(0);
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
