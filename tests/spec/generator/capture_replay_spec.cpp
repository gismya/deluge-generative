// Capture's replay (spec §17.1, §17.5, §17.6, §21.2): a piece's steering log played through the portable Compose
// driver gives what the live steered Perform run played, and the song's changes partway through the piece.

#include "compose_helpers.h"
#include "generative/compose/compose_driver.h"
#include "harness.h"
#include "stream_helpers.h"

#include "cppspec.hpp"

#include <memory>
#include <vector>

using namespace harness;
using namespace deluge::generative::compose;

namespace {

/// A log as Perform's: room for every piece a spec plays.
struct Log {
	std::vector<uint8_t> bytes = std::vector<uint8_t>(kPerformLogBytes);
	SteeringLog log{bytes.data(), kPerformLogBytes};
};

/// A piece's replay, one step a call as a runner job makes them in chunks.
struct Replayed {
	RecordingWriter writer;
	int32_t endTick = 0;
	uint32_t unpaired = 0;
	bool done = false;
};

std::unique_ptr<Replayed> replay(const SteeringLog::Piece& piece, int32_t riserMicros = 0) {
	auto out = std::make_unique<Replayed>();
	out->writer.riserMicros = riserMicros;
	auto generator = std::make_unique<Generator>();
	ComposeDriver driver;
	driver.beginReplay(*generator, piece, out->writer);
	for (int32_t calls = 0; calls < 20000 && !out->done; calls++) {
		out->done = driver.run(1);
	}
	out->endTick = driver.endTick();
	out->unpaired = driver.clips().unpaired();
	return out;
}

/// The notes both runs decided: those starting before the last decided step's decision tick, cut there.
int32_t agreedTick(const SteeringLog::Piece& piece) {
	return decisionTick(piece.endStep() - 1);
}

/// A live run steered by `commands`, as Techno from its Intro (seed 3), and the log it wrote.
struct Steered {
	Log log;
	Result live;
};

std::unique_ptr<Steered> steered(int32_t bars, std::vector<std::pair<int32_t, Command>> commands,
                                 bool skipIntro = false, uint32_t seed = 3) {
	auto out = std::make_unique<Steered>();
	Run run;
	run.recipe = acidRecipe();
	run.seed = seed;
	run.skipIntro = skipIntro;
	run.log = &out->log.log;
	run.steer = steerAt(std::move(commands));
	out->live = play(run, bars);
	return out;
}

} // namespace

// clang-format off
describe capture_replay("Capture replay (spec §17.1, §21.2)", ${
	context("a replay matches the live steered run", _ {
		it("with no steering", _ {
			auto run = steered(24, {});
			expect(run->log.log.pieces()).to_equal(1);
			const SteeringLog::Piece piece = run->log.log.piece(0);
			auto replayed = replay(piece);
			expect(replayed->done).to_be_true();
			expect(replayed->unpaired).to_equal(0U);
			const int32_t cut = agreedTick(piece);
			const std::vector<Note> live = notesBefore(pairNotes(run->live.events), cut);
			expect(live.size()).to_be_greater_than(100U);
			expect(notesBefore(replayed->writer.notes, cut) == live).to_be_true();
		});

		it("with forces, End and holds pressed mid-phrase", _ {
			auto run = steered(80, {{37, force(SectionKind::Break)},
			                        {101, force(SectionKind::Build)},
			                        {150, {CommandKind::SetHold, 0, kPress}},
			                        {190, {CommandKind::SetHold, 0, kPress}},
			                        {230, {CommandKind::End, 0, 1}}});
			const SteeringLog::Piece piece = run->log.log.piece(0);
			auto replayed = replay(piece);
			expect(replayed->done).to_be_true();
			const int32_t cut = agreedTick(piece);
			const std::vector<Note> live = notesBefore(pairNotes(run->live.events), cut);
			expect(notesBefore(replayed->writer.notes, cut) == live).to_be_true();
		});
		// The Arc's look-ahead reads a force, End or transition while it waits (the chord walk, drop-outs, Throws), so
		// the log says when each starts and stops waiting, and replay waits it from there (spec §2.6). Without that,
		// Dub's chord walk differed in the phrase before a forced Build.
		it("with a force or End pressed at any step, or pressed and cancelled, in every genre", _ {
			int32_t runs = 0;
			int32_t differ = 0;
			for (const Genre* genre : allGenres()) {
				for (int32_t kind = 0; kind < 5; kind++) {
					for (int32_t at = 20; at < 200; at += 11) {
						std::vector<std::pair<int32_t, Command>> commands;
						const Command press = kind == 0   ? force(SectionKind::Break)
						                      : kind == 1 ? force(SectionKind::Build)
						                      : kind == 2 ? Command{CommandKind::End, 0, 1}
						                                  : force(SectionKind::Groove);
						commands.push_back({at, press});
						if (kind == 4) {
							// A Build pressed, then pressed again a few steps later: cancelled while it waited.
							commands = {{at, force(SectionKind::Build)}, {at + 5, force(SectionKind::Build)}};
						}
						auto out = std::make_unique<Steered>();
						Run run;
						run.recipe = newRecipe(*genre, 0);
						run.seed = 3;
						run.log = &out->log.log;
						run.steer = steerAt(commands);
						out->live = play(run, 64);
						const SteeringLog::Piece piece = out->log.log.piece(0);
						auto replayed = replay(piece);
						const int32_t cut = agreedTick(piece);
						runs++;
						differ += notesBefore(replayed->writer.notes, cut) == notesBefore(pairNotes(out->live.events), cut)
						              ? 0
						              : 1;
					}
				}
			}
			expect(runs).to_be_greater_than(500);
			expect(differ).to_equal(0);
		});
	});
});

CPPSPEC_SPEC(capture_replay);
