// Capture's replay (spec §17.1, §17.5, §17.6, §21.2): a piece's steering log played through the portable Compose
// driver gives what the live steered Perform run played, and the song's changes partway through the piece.

#include "compose_helpers.h"
#include "generative/compose/capture_timing.h"
#include "generative/compose/compose_driver.h"
#include "generative/compose/song_size.h"
#include "generative/core/output_settings.h"
#include "harness.h"
#include "stream_helpers.h"
#include "transition_piece.h"

#include "cppspec.hpp"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <set>
#include <vector>

using namespace harness;
using namespace deluge::generative::compose;

namespace {

/// A log as Perform's: room for every piece a spec plays.
struct Log : StoredLog {};

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
	auto generator = std::make_unique<harness::Instance>();
	ComposeDriver driver;
	driver.beginReplay(*generator, piece, out->writer);
	for (int32_t calls = 0; calls < 20000 && !out->done; calls++) {
		out->done = driver.run(1);
	}
	out->endTick = driver.endTick();
	out->unpaired = driver.clips().unpaired();
	return out;
}

/// The notes both runs decided: those starting before the span's last step's decision tick, cut there.
int32_t agreedTick(const SteeringLog::Piece& piece) {
	return decisionTick(piece.spanEnd() - 1);
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

/// The notes one instance played in a TransitionPiece, as a piece of its own: the outgoing one's (`incoming` false,
/// with what it drained after the start step) or the incoming one's.
std::vector<Note> notesOf(const TransitionSink& sink, bool incoming) {
	std::vector<Event> events;
	for (const Played& p : sink.played) {
		if (p.incoming == incoming) {
			events.push_back(p.event);
		}
	}
	return pairNotes(events);
}

constexpr uint8_t kDrums = 0; ///< Techno Acid's DRUMS: a Kit part.
constexpr uint8_t kAcid = 1;  ///< Techno Acid's ACID: a synth part.

std::vector<LogRecord> recordsOf(const SteeringLog::Piece& piece) {
	std::vector<LogRecord> out;
	piece.forEachRecord([&](const LogRecord& r) { out.push_back(r); });
	return out;
}

bool hasRecord(const std::vector<LogRecord>& read, RecordKind kind, CommandKind command) {
	return std::any_of(read.begin(), read.end(),
	                   [&](const LogRecord& r) { return r.kind == kind && r.command.kind == command; });
}

/// Notes as a song at swing `songSwing` holds those played at `swing` from `from` on: each end moved by its swing
/// difference (spec §17.6), on the Recipe's swing interval.
std::vector<Note> swungFrom(std::vector<Note> notes, int32_t from, int32_t swing, int32_t songSwing, int32_t interval) {
	for (Note& n : notes) {
		const auto shift = [&](int32_t tick) {
			return stepTick(tick / kTicksPerStep) >= from ? swingShifted(tick, swing, interval, songSwing, interval)
			                                              : tick;
		};
		const int32_t end = shift(n.tick + n.length);
		n.tick = shift(n.tick);
		n.length = end - n.tick;
	}
	return notes;
}

/// A StepwisePiece with a log as Perform's.
struct Logged : StepwisePiece<> {
	explicit Logged(const Start& start) : StepwisePiece(start) { driver.setLog(&own.log); }
	Log own;
};

} // namespace

// clang-format off
describe capture_replay("Capture replay (spec §17.1, §21.2)", ${
	context("a replay matches the live steered run", _ {
		// Every command kind, a transition with its tempo glide, a re-roll, a dropped swap and a key change, each
		// piece's replay against what its instance played (spec §21.2).
		it("with every command kind, a transition, a re-roll, a dropped swap, a key change and a tempo glide", _ {
			Recipe playing = acidRecipe();
			Recipe pending = acidRecipe();
			pending.tempo = playing.tempo + 1200;
			TransitionPiece t(playing, pending);
			Log log;
			t.driver.setLog(&log.log);
			const int32_t phrase = t.phraseSteps();

			t.decideBefore(2);
			log.log.mark(true); // the outgoing piece is kept past the transition
			t.push({CommandKind::SetMacro, static_cast<uint8_t>(Macro::Energy), macroPoints(70)});
			t.push({CommandKind::SetBias, kAcid, macroPoints(20), static_cast<uint16_t>(Macro::Density)});
			t.push({CommandKind::SetOctave, kAcid, 1});
			t.push({CommandKind::SetTake, 0, 1});
			t.push(mute(2));
			t.push({CommandKind::SetPunch, static_cast<uint8_t>(PunchIn::HatRoll), 1});
			t.push({CommandKind::SetStutter, 0, 1});
			t.decideBefore(5);
			t.push({CommandKind::SetKey, 5, static_cast<int32_t>(Scale::Dorian)});
			t.push({CommandKind::SetTempo, 0, playing.tempo + 600, static_cast<uint16_t>(kBarTicks)});
			t.push({CommandKind::SetSwing, 0, playing.swing});
			t.push({CommandKind::SetSwingInterval, 0, playing.swingInterval});
			t.push({CommandKind::SetHome, kAcid, 30, 4});
			t.push({CommandKind::SetParam, kSongParams, 900, 7});
			t.push({CommandKind::SetSetting, kAcid, 2, static_cast<uint16_t>(OutputSetting::LpfMode), Lane::None});
			t.decideBefore(9);
			t.push({CommandKind::SetFreeze, 3, kPress});
			t.push(mute(0, true));
			t.push({CommandKind::SetSolo, 0, kPressNow});
			t.push({CommandKind::SetHold, 0, kPress});
			t.push({CommandKind::SetFill, 0, kFillHeldEntry});
			t.push({CommandKind::SetStutter, 0, 0});
			t.decideBefore(13);
			t.push({CommandKind::SetFill, 0, kFillReleased});
			t.push({CommandKind::SetSolo, 0, kPressNow});
			t.push({CommandKind::SetFreeze, 3, kPress});
			t.push({CommandKind::SetHold, 0, kPress});
			t.push({CommandKind::SetPunch, static_cast<uint8_t>(PunchIn::HatRoll), 0});
			t.push(force(SectionKind::Break));
			t.push({CommandKind::SetMacro, static_cast<uint8_t>(Macro::Motion), macroPoints(80)});
			// A re-roll that lands on the next phrase line.
			t.sounds.press(kAcid, false);
			t.sounds.endPreload();
			t.runToStepLine(phrase + 2);
			// A second, dropped by the transition's confirm between its landing and its Sound event (spec §17.2).
			t.sounds.press(kAcid, false);
			t.sounds.endPreload();
			t.runToStepLine(2 * phrase, -1);
			t.lifecycle.request(TransitionStyle::BuildIntoDrop);
			t.runToStepLine(2 * phrase, kTicksPerStep);
			t.lifecycle.load();
			t.runBars(3 * phrase / kStepsPerBar);
			expect(t.sink.starts).to_equal(1);
			// In the incoming piece: End, and its Outro ends it.
			t.push({CommandKind::End, 0, 1});
			t.runBars(4 * phrase / kStepsPerBar);
			t.stop();

			expect(log.log.pieces()).to_equal(2);
			const PieceImage outImage = image(log.log, 0);
			const SteeringLog::Piece out = outImage.piece();
			const PieceImage inImage = image(log.log, 1);
			const SteeringLog::Piece in = inImage.piece();
			expect(out.ended() == PieceEnd::Transition).to_be_true();
			// Every command kind but those never logged.
			std::vector<bool> kinds(static_cast<size_t>(kLastCommandKind) + 1);
			bool dropped = false;
			for (const SteeringLog::Piece& piece : {out, in}) {
				piece.forEachRecord([&](const LogRecord& r) {
					if (r.kind == RecordKind::Command) {
						kinds[static_cast<size_t>(r.command.kind)] = true;
					}
					dropped = dropped || r.kind == RecordKind::SoundDropped;
				});
			}
			for (size_t k = 0; k < kinds.size(); k++) {
				const auto kind = static_cast<CommandKind>(k);
				if (kind != CommandKind::CancelNewest && kind != CommandKind::Restart) {
					expect(static_cast<bool>(kinds[k])).to_be_true();
				}
			}
			expect(dropped).to_be_true();

			auto outgoing = replay(out);
			expect(outgoing->done).to_be_true();
			expect(outgoing->unpaired).to_equal(0U);
			const int32_t outCut = agreedTick(out);
			const std::vector<Note> outLive = notesBefore(notesOf(t.sink, false), outCut);
			expect(outLive.size()).to_be_greater_than(50U);
			expect(notesBefore(outgoing->writer.notes, outCut) == outLive).to_be_true();
			auto incoming = replay(in);
			expect(incoming->done).to_be_true();
			const int32_t inCut = agreedTick(in);
			const std::vector<Note> inLive = notesBefore(notesOf(t.sink, true), inCut);
			expect(inLive.size()).to_be_greater_than(50U);
			expect(notesBefore(incoming->writer.notes, inCut) == inLive).to_be_true();

			// The song's changes partway through (spec §17.6): the landed re-roll's new track and not the dropped
			// one's, the key change's cut on each pitched clip, the tempo glides, the param edits and the stutter.
			const RecordingWriter& w = outgoing->writer;
			expect(w.tracks.size()).to_equal(1U);
			expect(w.tracks.empty() || w.tracks[0].part == kAcid).to_be_true();
			expect(w.tracks.empty() || w.tracks[0].tick == stepTick(phrase)).to_be_true();
			expect(w.cuts.empty()).to_be_false();
			expect(std::all_of(w.cuts.begin(), w.cuts.end(), [&](const auto& cut) {
				return playing.genre->parts[cut.first].synth() && cut.second == w.cuts[0].second;
			})).to_be_true();
			expect(w.tempos.size()).to_be_greater_than(2U);
			expect(w.tempos.empty() || w.tempos.back().tempo == pending.tempo).to_be_true();
			expect(w.edits.size()).to_equal(2U);
			expect(w.settings.size()).to_equal(1U);
			expect(w.stutters.size()).to_equal(2U);
		});

		it("with no steering", _ {
			auto run = steered(24, {});
			expect(run->log.log.pieces()).to_equal(1);
			const PieceImage pieceImage = image(run->log.log, 0);
			const SteeringLog::Piece piece = pieceImage.piece();
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
			const PieceImage pieceImage = image(run->log.log, 0);
			const SteeringLog::Piece piece = pieceImage.piece();
			auto replayed = replay(piece);
			expect(replayed->done).to_be_true();
			const int32_t cut = agreedTick(piece);
			const std::vector<Note> live = notesBefore(pairNotes(run->live.events), cut);
			expect(notesBefore(replayed->writer.notes, cut) == live).to_be_true();
		});
		it("with a hard cut, whose Transition sits on the outgoing piece's last step", _ {
			TransitionPiece t;
			Log log;
			t.driver.setLog(&log.log);
			const int32_t phrase = t.phraseSteps();
			t.decideBefore(2);
			log.log.mark(true);
			t.confirmAt(phrase + 5, TransitionStyle::Cut);
			t.runBars(2 * phrase / kStepsPerBar);
			expect(t.sink.starts).to_equal(1);
			t.stop();
			const PieceImage outImage = image(log.log, 0);
			const SteeringLog::Piece out = outImage.piece();
			expect(out.ended() == PieceEnd::Transition).to_be_true();
			bool onEnd = false;
			out.forEachRecord([&](const LogRecord& r) {
				onEnd = onEnd || (r.kind == RecordKind::Command && r.command.kind == CommandKind::Transition
				                  && r.step == out.endStep());
			});
			expect(onEnd).to_be_true();
			auto replayed = replay(out);
			expect(replayed->done).to_be_true();
			expect(replayed->unpaired).to_equal(0U);
			expect(replayed->endTick).to_equal(stepTick(out.endStep()));
			// Every note ends by the cut line, as live (spec §12.9).
			const int32_t cut = stepTick(out.endStep());
			expect(std::all_of(replayed->writer.notes.begin(), replayed->writer.notes.end(),
			                   [&](const Note& n) { return n.tick + n.length <= cut + kMaxLean; }))
			    .to_be_true();
			const std::vector<Note> live = notesBefore(notesOf(t.sink, false), agreedTick(out));
			expect(notesBefore(replayed->writer.notes, agreedTick(out)) == live).to_be_true();
		});

		it("with a restart, each piece on its own (spec §13.2)", _ {
			Logged piece({.seed = 3, .skipIntro = true});
			piece.decideBefore(2);
			piece.own.log.mark(true);
			piece.push({CommandKind::SetMacro, static_cast<uint8_t>(Macro::Density), macroPoints(80)});
			piece.decideBefore(37);
			piece.push({CommandKind::Restart, 0, 1});
			piece.hook.runUntil([&] { return !piece.sink.restartedAt.empty(); });
			piece.runBars(6);
			piece.driver.stop(piece.sink);
			// The restart ends the piece on its line; once #413 part 2 lands, the restarted run opens a piece of its own.
			expect(piece.own.log.pieces() >= 1).to_be_true();
			expect(piece.sink.restartedAt.size()).to_equal(1U);
			if (piece.sink.restartedAt.size() != 1U) {
				return;
			}
			expect(piece.own.log.piece(0).endStep()).to_be_greater_than(37);
			const size_t restartAt = piece.sink.restartedAt[0];
			const std::vector<Event> first(piece.sink.events.begin(), piece.sink.events.begin() + restartAt);
			const std::vector<Event> second(piece.sink.events.begin() + restartAt, piece.sink.events.end());
			for (int32_t i = 0; i < std::min<int32_t>(piece.own.log.pieces(), 2); i++) {
				const PieceImage loggedImage = image(piece.own.log, i);
				const SteeringLog::Piece logged = loggedImage.piece();
				auto replayed = replay(logged);
				expect(replayed->done).to_be_true();
				const int32_t cut = agreedTick(logged);
				const std::vector<Note> live = notesBefore(pairNotes(i == 0 ? first : second), cut);
				expect(live.size()).to_be_greater_than(20U);
				expect(notesBefore(replayed->writer.notes, cut) == live).to_be_true();
			}
		});

		it("up to where a full log cut the piece (spec §17.2)", _ {
			// A ring with room for a few dozen macro turns, and a card that stalls: the ring fills partway through.
			StoredLog stalls(kLogHeaderBytes, 10 * kMaxCommandBytes + kDroppedReserve);
			stalls.store.stalled = true;
			SteeringLog& small = stalls.log;
			std::vector<std::pair<int32_t, Command>> turns;
			for (int32_t step = 4; step < 300; step += 3) {
				turns.push_back({step, {CommandKind::SetMacro, static_cast<uint8_t>(Macro::Energy), macroPoints(step % 90)}});
			}
			Run run;
			run.recipe = acidRecipe();
			run.seed = 3;
			run.log = &small;
			run.steer = steerAt(turns);
			const Result live = play(run, 32);
			stalls.store.stalled = false;
			const PieceImage pieceImage = image(small, 0);
			const SteeringLog::Piece piece = pieceImage.piece();
			expect(piece.cutAt()).to_be_greater_than(10);
			auto replayed = replay(piece);
			expect(replayed->done).to_be_true();
			expect(replayed->endTick).to_equal(stepTick(piece.cutAt()));
			const int32_t cut = agreedTick(piece);
			expect(notesBefore(replayed->writer.notes, cut) == notesBefore(pairNotes(live.events), cut)).to_be_true();
		});

		it("as a Compose of the same length when Perform plays untouched but for End (spec §17.5)", _ {
			const Recipe recipe = acidRecipe();
			RecordingWriter composed;
			{
				auto generator = std::make_unique<harness::Instance>();
				ComposeDriver driver;
				driver.begin(*generator, recipe, 3, Takes{}, targetSteps(60, recipe.tempo), composed);
				while (!driver.run(64)) {}
				// Perform presses End where Compose queued it.
				auto run = std::make_unique<Steered>();
				Run live;
				live.recipe = recipe;
				live.seed = 3;
				live.log = &run->log.log;
				live.steer = steerAt({{driver.endQueuedAt(), {CommandKind::End, 0, 1}}});
				run->live = play(live, driver.endTick() / kBarTicks + 4);
				expect(run->live.ended).to_be_true();
				const PieceImage pieceImage = image(run->log.log, 0);
				const SteeringLog::Piece piece = pieceImage.piece();
				expect(piece.ended() == PieceEnd::Outro).to_be_true();
				auto replayed = replay(piece);
				expect(replayed->endTick).to_equal(driver.endTick());
				expect(replayed->writer.notes == composed.notes).to_be_true();
				expect(replayed->writer.nodes == composed.nodes).to_be_true();
				expect(replayed->writer.bases == composed.bases).to_be_true();
				expect(replayed->writer.tempos.empty()).to_be_true();
				expect(replayed->writer.songs.empty()).to_be_true();
			}
		});

		// The Arc's look-ahead reads a force, End or transition while it waits (the chord walk, drop-outs, Throws), so
		// the log says when each starts and stops waiting, and replay waits it from there (spec §2.6). Without that,
		// Dub's chord walk differed in the phrase before a forced Build.
		it("with a waiting transition cancelled: its slot empties (NotWaiting) and the piece plays on", _ {
			TransitionPiece t;
			Log log;
			t.driver.setLog(&log.log);
			t.confirmAt(3, TransitionStyle::BuildIntoDrop);
			t.decideBefore(8);
			expect(t.out->transitionWaiting()).to_be_true();
			expect(t.lifecycle.cancel()).to_be_true();
			t.runBars(3 * t.phraseSteps() / kStepsPerBar);
			expect(t.sink.starts).to_equal(0);
			t.stop();
			const PieceImage pieceImage = image(log.log, log.log.pieces() - 1);
			const SteeringLog::Piece piece = pieceImage.piece();
			const std::vector<LogRecord> read = recordsOf(piece);
			expect(hasRecord(read, RecordKind::Waiting, CommandKind::Transition)).to_be_true();
			expect(hasRecord(read, RecordKind::NotWaiting, CommandKind::Transition)).to_be_true();
			expect(hasRecord(read, RecordKind::Command, CommandKind::Transition)).to_be_false();
			auto replayed = replay(piece);
			const int32_t cut = agreedTick(piece);
			const std::vector<Note> live = notesBefore(notesOf(t.sink, false), cut);
			expect(live.size()).to_be_greater_than(100U);
			expect(notesBefore(replayed->writer.notes, cut) == live).to_be_true();
		});

		it("with End tapped again to cancel it while it waits", _ {
			auto run = steered(48, {{20, {CommandKind::End, 0, 1}}, {25, {CommandKind::End, 0, 0}}}, true);
			const PieceImage pieceImage = image(run->log.log, 0);
			const SteeringLog::Piece piece = pieceImage.piece();
			const std::vector<LogRecord> read = recordsOf(piece);
			expect(hasRecord(read, RecordKind::Waiting, CommandKind::End)).to_be_true();
			expect(hasRecord(read, RecordKind::NotWaiting, CommandKind::ForceSection)).to_be_true();
			expect(hasRecord(read, RecordKind::Command, CommandKind::End)).to_be_false();
			auto replayed = replay(piece);
			const int32_t cut = agreedTick(piece);
			expect(notesBefore(replayed->writer.notes, cut) == notesBefore(pairNotes(run->live.events), cut))
			    .to_be_true();
		});

		it("with a waiting force replaced by End in their shared slot", _ {
			auto run = steered(64, {{20, force(SectionKind::Break)}, {25, {CommandKind::End, 0, 1}}}, true);
			const PieceImage pieceImage = image(run->log.log, 0);
			const SteeringLog::Piece piece = pieceImage.piece();
			const std::vector<LogRecord> read = recordsOf(piece);
			expect(hasRecord(read, RecordKind::Waiting, CommandKind::ForceSection)).to_be_true();
			expect(hasRecord(read, RecordKind::Waiting, CommandKind::End)).to_be_true();
			expect(hasRecord(read, RecordKind::Command, CommandKind::ForceSection)).to_be_false();
			expect(hasRecord(read, RecordKind::Command, CommandKind::End)).to_be_true();
			auto replayed = replay(piece);
			const int32_t cut = agreedTick(piece);
			expect(notesBefore(replayed->writer.notes, cut) == notesBefore(pairNotes(run->live.events), cut))
			    .to_be_true();
		});

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
						const PieceImage pieceImage = image(out->log.log, 0);
						const SteeringLog::Piece piece = pieceImage.piece();
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

	context("changes partway through a piece (spec §17.6)", _ {
		it("builds a swing change into the ticks of the notes played after it", _ {
			const Recipe recipe = acidRecipe();
			const int32_t swing = recipe.swing + 20;
			auto run = steered(24, {{40, {CommandKind::SetSwing, 0, swing}}}, true);
			const PieceImage pieceImage = image(run->log.log, 0);
			const SteeringLog::Piece piece = pieceImage.piece();
			auto replayed = replay(piece);
			const int32_t cut = agreedTick(piece);
			const std::vector<Note> live = notesBefore(pairNotes(run->live.events), cut);
			const std::vector<Note> song = notesBefore(replayed->writer.notes, cut);
			expect(song == live).to_be_false();
			const std::vector<Note> expected =
			    swungFrom(notesBefore(pairNotes(run->live.events), cut), stepTick(40), swing, recipe.swing,
			              recipe.swingInterval);
			expect(song == notesBefore(expected, cut)).to_be_true();
		});

		it("holds a note's tick at another swing where it sounds the same", _ {
			for (int32_t interval = 0; interval < 3; interval++) {
				const int32_t pair = swingPairTicks(interval);
				for (int32_t tick = 0; tick < 3 * pair; tick++) {
					expect(swingShifted(tick, 17, interval, 17, interval)).to_equal(tick);
				}
				// A pair's lines never move; its midpoint moves by the swing difference.
				expect(swingShifted(pair, 30, interval, 0, interval)).to_equal(pair);
				expect(swingShifted(pair / 2, 30, interval, 0, interval)).to_equal((pair / 2 * 80 + 25) / 50);
				expect(std::abs(swingShifted((pair / 2 * 80 + 25) / 50, 0, interval, 30, interval) - pair / 2) <= 1).to_be_true();
			}
		});

		it("writes tempo glides as tempo automation, in tick order, ending where each glide ends", _ {
			const Recipe recipe = acidRecipe();
			auto run = steered(24, {{20, {CommandKind::SetTempo, 0, recipe.tempo + 1000, static_cast<uint16_t>(kBarTicks)}},
			                        {60, {CommandKind::SetTempo, 0, recipe.tempo - 500}},
			                        {80, {CommandKind::SetTempo, 0, recipe.tempo, static_cast<uint16_t>(4 * kBarTicks)}},
			                        {90, {CommandKind::SetTempo, 0, recipe.tempo + 300, static_cast<uint16_t>(kBarTicks)}}},
			                   true);
			auto replayed = replay(image(run->log.log, 0).piece());
			const std::vector<TempoNode> expected = {
			    {0, recipe.tempo, false}, // the song's own tempo, from its start
			    {stepTick(20), recipe.tempo, false},
			    {stepTick(20) + kBarTicks, recipe.tempo + 1000, true},
			    {stepTick(60), recipe.tempo - 500, false},
			    {stepTick(80), recipe.tempo - 500, false},
			    // Cut short by the next glide: it holds what it reached there.
			    {stepTick(90), recipe.tempo - 500 + 500 * (stepTick(90) - stepTick(80)) / (4 * kBarTicks), true},
			    {stepTick(90) + kBarTicks, recipe.tempo + 300, true},
			};
			expect(replayed->writer.tempos == expected).to_be_true();
		});

		it("starts a new track at each re-roll that played, with its picks, and keeps the old one for a dropped swap", _ {
			StoredLog stored;
			SteeringLog& log = stored.log;
			PieceStart start;
			start.recipe = acidRecipe();
			start.seed = 3;
			start.first = SectionKind::Groove;
			log.beginPiece(start);
			const int32_t phrase = start.recipe.genre->phraseSteps();
			log.append(phrase, {CommandKind::SetSound, kAcid, 4});
			log.appendSoundPath(phrase, kAcid, 4, pickSlot(kAcid), "SYNTHS/ACID 4.XML");
			log.append(2 * phrase, {CommandKind::SetSound, kAcid, 5});
			log.appendSoundPath(2 * phrase, kAcid, 5, pickSlot(kAcid), "SYNTHS/ACID 5.XML");
			log.appendSoundDropped(2 * phrase, kAcid);
			log.endPiece(3 * phrase, PieceEnd::Stopped);
			auto replayed = replay(image(log, 0).piece());
			const RecordingWriter& w = replayed->writer;
			expect(w.picks.size()).to_equal(2U);
			expect(w.picks.size() == 2U && w.picks[0].path == "SYNTHS/ACID 4.XML" && w.picks[0].token == 4).to_be_true();
			expect(w.tracks == std::vector<RecordingWriter::Track>{{kAcid, 4, stepTick(phrase)}}).to_be_true();
		});

		it("starts a new Kit track at a hand-chosen lane swap, logging every lane's path and cutting the Kit's lanes there (#300)", _ {
			Recipe playing = acidRecipe();
			playing.macros.set(Macro::Motion, kMacroFull); // Kit lanes move, checking cutPart() leaves no late nodes
			TransitionPiece t(playing, playing);
			Log log;
			t.driver.setLog(&log.log);
			const int32_t phrase = t.phraseSteps();

			// Entry picks for every DRUMS lane, then a hand-chosen sample for Kick alone (spec §10.1, §17.6).
			SoundPicks entryPicks;
			for (const LaneSpec& lane : playing.genre->parts[kDrums].lanes) {
				entryPicks.lane[static_cast<int32_t>(lane.lane)].set(
				    "SAMPLES/DRUMS", ("ENTRY_" + std::to_string(static_cast<int32_t>(lane.lane)) + ".wav").c_str());
			}
			SoundPicks waitingPicks = entryPicks;
			SoundChoice choice;
			choice.path.set("SAMPLES/MINE/ChosenKick.wav");
			choice.lane = Lane::Kick;
			expect(chooseSound(*playing.genre, kDrums, choice, waitingPicks)).to_be_true();

			// Mirrors firmware OutputSink::logSoundPaths() -> firmware::logSoundPaths(): every lane of the Kit logs.
			t.sink.onLogSoundPaths = [&](SteeringLog& target, int32_t step, uint8_t part, uint8_t token) {
				for (const LaneSpec& lane : playing.genre->parts[part].lanes) {
					const SoundPath& path = waitingPicks.lane[static_cast<int32_t>(lane.lane)];
					if (!path.empty()) {
						target.appendSoundPath(step, part, token, pickSlot(lane.lane), path.c_str());
					}
				}
			};

			t.decideBefore(4);
			expect(t.sounds.choose(kDrums, choice) == perform::SoundPress::Preloading).to_be_true();
			t.sounds.endPreload();
			t.runToStepLine(phrase + 4);
			// A second lane swap on DRUMS dropped by a transition confirm keeps the track from the first swap.
			SoundChoice droppedChoice;
			droppedChoice.path.set("SAMPLES/MINE/DroppedKick.wav");
			droppedChoice.lane = Lane::Kick;
			expect(chooseSound(*playing.genre, kDrums, droppedChoice, waitingPicks)).to_be_true();
			expect(t.sounds.choose(kDrums, droppedChoice) == perform::SoundPress::Preloading).to_be_true();
			t.sounds.endPreload();
			t.runToStepLine(2 * phrase, -1);
			t.lifecycle.request(TransitionStyle::BuildIntoDrop);
			t.runToStepLine(2 * phrase, kTicksPerStep);
			t.stop();

			const PieceImage img = image(log.log, 0);
			const SteeringLog::Piece piece = img.piece();
			auto replayed = replay(piece);
			expect(replayed->done).to_be_true();
			expect(replayed->unpaired).to_equal(0U);
			const RecordingWriter& w = replayed->writer;
			const uint8_t firstToken = t.sounds.preloads[0].token;
			expect(w.tracks == std::vector<RecordingWriter::Track>{{kDrums, firstToken, stepTick(phrase)}}).to_be_true();
			expect(w.lateNodes).to_equal(0);
			// The chosen Kick path and every other DRUMS lane's unchanged entry path were logged and delivered to Capture.
			const auto hasPick = [&](Lane lane, const std::string& path) {
				const int32_t slot = pickSlot(lane);
				return std::any_of(w.picks.begin(), w.picks.end(), [&](const RecordingWriter::Pick& p) {
					return p.part == kDrums && p.token == firstToken && p.slot == slot && p.path == path;
				});
			};
			expect(hasPick(Lane::Kick, "SAMPLES/MINE/ChosenKick.wav")).to_be_true();
			for (const LaneSpec& lane : playing.genre->parts[kDrums].lanes) {
				if (lane.lane != Lane::Kick) {
					expect(hasPick(lane.lane, entryPicks.lane[static_cast<int32_t>(lane.lane)].c_str())).to_be_true();
				}
			}
			const int32_t cut = agreedTick(piece);
			expect(notesBefore(w.notes, cut) == notesBefore(notesOf(t.sink, false), cut)).to_be_true();
		});

		it("ends a part's lanes where its clip is cut or its new track starts: no node for the part comes late", _ {
			StoredLog stored;
			SteeringLog& log = stored.log;
			PieceStart start;
			start.recipe = acidRecipe();
			start.recipe.macros.set(Macro::Motion, kMacroFull); // lanes that move all the time
			start.seed = 3;
			start.first = SectionKind::Groove;
			log.beginPiece(start);
			const int32_t phrase = start.recipe.genre->phraseSteps();
			log.append(phrase, {CommandKind::SetKey, 2, static_cast<int32_t>(Scale::Dorian)});
			log.append(2 * phrase + 3, {CommandKind::SetSound, kAcid, 4});
			log.appendSoundPath(2 * phrase + 3, kAcid, 4, pickSlot(kAcid), "SYNTHS/ACID 4.XML");
			log.endPiece(4 * phrase, PieceEnd::Stopped);
			auto replayed = replay(image(log, 0).piece());
			const RecordingWriter& w = replayed->writer;
			expect(w.cuts.empty()).to_be_false();
			expect(w.tracks.size()).to_equal(1U);
			expect(w.lateNodes).to_equal(0);
			const bool after = std::any_of(w.nodes.begin(), w.nodes.end(), [&](const auto& node) {
				return node.first.part == kAcid && node.second.tick >= stepTick(2 * phrase + 3);
			});
			expect(after).to_be_true();
		});

		it("writes a moved home's lanes again where it moves: each lane of the part that moved restates its value", _ {
			StoredLog stored;
			SteeringLog& log = stored.log;
			PieceStart start;
			start.recipe = acidRecipe();
			start.recipe.macros.set(Macro::Motion, kMacroFull);
			start.seed = 3;
			start.first = SectionKind::Groove;
			log.beginPiece(start);
			const int32_t phrase = start.recipe.genre->phraseSteps();
			const int32_t at = 2 * phrase + 3;
			log.append(at, {CommandKind::SetHome, kAcid, 40, 0x0005, Lane::None});
			log.endPiece(3 * phrase, PieceEnd::Stopped);
			auto replayed = replay(image(log, 0).piece());
			const RecordingWriter& w = replayed->writer;
			expect(w.edits.size()).to_equal(1U);
			expect(w.editNodes.size()).to_equal(1U);
			const int32_t tick = stepTick(at);
			std::vector<Automated> before;
			std::vector<Automated> restated;
			for (size_t i = 0; i < w.nodes.size(); i++) {
				const auto& [id, node] = w.nodes[i];
				if (id.part != kAcid) {
					continue;
				}
				if (i < w.editNodes[0] && std::find(before.begin(), before.end(), id) == before.end()) {
					before.push_back(id);
				}
				if (i >= w.editNodes[0] && node.tick == tick) {
					restated.push_back(id);
				}
				// Nothing for the part comes after the edit with a tick before it.
				expect(i < w.editNodes[0] || node.tick >= tick).to_be_true();
			}
			expect(before.empty()).to_be_false();
			for (const Automated& id : before) {
				expect(std::find(restated.begin(), restated.end(), id) != restated.end()).to_be_true();
			}
		});

		it("splits a song too big to load back at a Section line, and the next starts with that Section (§17.5)", _ {
			for (const Genre* genre : {&genres::technoAcid, &genres::ambient}) {
				Run run;
				run.recipe = newRecipe(*genre, 0);
				run.seed = 3;
				Log log;
				run.log = &log.log;
				const Result live = play(run, 96);
				const PieceImage pieceImage = image(log.log, 0);
				const SteeringLog::Piece piece = pieceImage.piece();
				// A budget a few bars past the empty song's.
				const int64_t budget = estimateBarsBytes(*genre, 20);
				auto writer = std::make_unique<RecordingWriter>();
				auto generator = std::make_unique<harness::Instance>();
				ComposeDriver driver;
				driver.beginReplay(*generator, piece, *writer, budget);
				while (!driver.run(64)) {}
				expect(driver.songs()).to_be_greater_than(1);
				expect(static_cast<int32_t>(writer->songs.size())).to_equal(driver.songs() - 1);
				std::set<int32_t> sections;
				for (const Event& e : live.events) {
					if (e.kind == EventKind::Section) {
						sections.insert(e.tick);
					}
				}
				expect(std::all_of(writer->songs.begin(), writer->songs.end(), [&](int32_t tick) {
					return sections.contains(tick);
				})).to_be_true();
				// The songs hold the live notes, a note held across a split ending there and struck again (a
				// Sustain's, spec §5.2).
				std::vector<Note> expected;
				for (Note n : pairNotes(live.events)) {
					for (int32_t split : writer->songs) {
						if (n.tick < split && n.tick + n.length > split) {
							expected.push_back({n.tick, split - n.tick, n.part, n.lane, n.pitch, n.velocity});
							n.length = n.tick + n.length - split;
							n.tick = split;
						}
					}
					expected.push_back(n);
				}
				const int32_t cut = agreedTick(piece);
				expect(notesBefore(writer->notes, cut) == notesBefore(expected, cut)).to_be_true();
			}
		});

		it("replays mid-piece SetSetting and SettingPath without altering the event stream, ending the part's lanes at the step", _ {
			StoredLog stored;
			SteeringLog& log = stored.log;
			PieceStart start;
			start.recipe = acidRecipe();
			start.recipe.macros.set(Macro::Motion, kMacroFull);
			start.seed = 3;
			start.first = SectionKind::Groove;
			log.beginPiece(start);
			const int32_t phrase = start.recipe.genre->phraseSteps();
			const int32_t at = phrase + 5;
			const Command oscType{CommandKind::SetSetting, kAcid, 7, static_cast<uint16_t>(OutputSetting::Osc0Type),
			                      Lane::None};
			const Command songMod{CommandKind::SetSetting, kSongParams, 2,
			                      static_cast<uint16_t>(OutputSetting::ModFxType), Lane::None};
			log.append(at, oscType);
			log.appendSettingPath(at, kAcid, Lane::None, static_cast<uint16_t>(OutputSetting::Osc0SamplePath),
			                      "SAMPLES/TEST.WAV");
			log.append(at + 4, songMod);
			log.endPiece(2 * phrase, PieceEnd::Stopped);

			// Replay matches an untouched run of the same Recipe and seed, while delivering the settings at their steps.
			Run run;
			run.recipe = start.recipe;
			run.seed = start.seed;
			run.skipIntro = true;
			const Result live = play(run, 2 * phrase / kStepsPerBar);

			const PieceImage img = image(log, 0);
			auto replayed = replay(img.piece());
			const RecordingWriter& w = replayed->writer;
			const int32_t cut = agreedTick(img.piece());
			expect(notesBefore(w.notes, cut) == notesBefore(pairNotes(live.events), cut)).to_be_true();
			expect(w.lateNodes).to_equal(0);
			expect(w.settings.size()).to_equal(2U);
			expect(w.settings[0].first == oscType && w.settings[0].second == stepTick(at)).to_be_true();
			expect(w.settings[1].first == songMod && w.settings[1].second == stepTick(at + 4)).to_be_true();
			expect(w.settingPaths.size()).to_equal(1U);
			expect(w.settingPaths[0]
			       == RecordingWriter::SettingPath{kAcid, Lane::None,
			                                       static_cast<uint16_t>(OutputSetting::Osc0SamplePath),
			                                       "SAMPLES/TEST.WAV", stepTick(at)})
			    .to_be_true();
		});

		it("replays ArpMain before a mid-piece key change (cutClip) and keeps same-step SetSetting order", _ {
			StoredLog stored;
			SteeringLog& log = stored.log;
			PieceStart start;
			start.recipe = acidRecipe();
			start.seed = 4;
			start.first = SectionKind::Groove;
			log.beginPiece(start);
			const int32_t phrase = start.recipe.genre->phraseSteps();
			const Command arpOn{CommandKind::SetSetting, kAcid, 1, static_cast<uint16_t>(OutputSetting::ArpMain),
			                    Lane::None};
			const Command keyChange{CommandKind::SetKey, 0, 5, static_cast<uint16_t>(Scale::Minor), Lane::None};
			const Command synthFm{CommandKind::SetSetting, kAcid, 1, static_cast<uint16_t>(OutputSetting::SynthMode),
			                      Lane::None};
			const Command lpfNotch{CommandKind::SetSetting, kAcid, 5, static_cast<uint16_t>(OutputSetting::LpfMode),
			                       Lane::None};
			log.append(12, arpOn);
			log.append(phrase, keyChange);
			log.append(phrase + 4, synthFm);
			log.append(phrase + 4, lpfNotch);
			log.endPiece(2 * phrase, PieceEnd::Stopped);

			const PieceImage img = image(log, 0);
			auto replayed = replay(img.piece());
			const RecordingWriter& w = replayed->writer;
			expect(w.settings.size()).to_equal(3U);
			expect(w.settings[0].first == arpOn && w.settings[0].second == stepTick(12)).to_be_true();
			expect(w.settings[1].first == synthFm && w.settings[1].second == stepTick(phrase + 4)).to_be_true();
			expect(w.settings[2].first == lpfNotch && w.settings[2].second == stepTick(phrase + 4)).to_be_true();
			bool cutAcidAfterArp = false;
			for (const auto& [part, tick] : w.cuts) {
				if (part == kAcid && tick == stepTick(phrase)) {
					cutAcidAfterArp = true;
				}
			}
			expect(cutAcidAfterArp).to_be_true();
		});
	});
});

CPPSPEC_SPEC(capture_replay);
