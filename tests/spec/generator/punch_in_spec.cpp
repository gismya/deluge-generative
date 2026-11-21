// The tier 2 punch-ins (spec §8.9): a SetPunch lands on the next undecided step and turns one on or off. A LaneMute
// flags one Drums lane's notes, the rolls replace the Closed hat's or the Snare's hits with 32nds or 16ths, Flam and
// Double kick replace the backbeat lanes' and the Kick's Ornaments, and Pump sends a pump event the driver ducks the
// other parts on. Each draws nothing, so the piece around it plays as it would have.

#include "harness.h"
#include "stream_helpers.h"

#include "cppspec.hpp"

#include <algorithm>
#include <cstdlib>
#include <vector>

using namespace harness;

namespace {

/// The punch-in goes on as step kOn is decided and off as step kOff is.
constexpr int32_t kOn = 2 * kStepsPerBar + 3;
constexpr int32_t kOff = 5 * kStepsPerBar + 5;
constexpr int32_t kBars = 7;
constexpr uint32_t kSeeds[] = {1, 2, 3};

Command punch(PunchIn which, bool on, Lane lane = Lane::None) {
	Command command{CommandKind::SetPunch, static_cast<uint8_t>(which), on ? 1 : 0};
	command.lane = lane;
	return command;
}

int32_t drumsOf(const Genre& genre) {
	for (int32_t p = 0; p < partCount(genre); p++) {
		if (genre.parts[p].role == Role::Drums) {
			return p;
		}
	}
	return -1;
}

/// `genre` from its first Groove with `seed`, the punch-in on from kOn to kOff (none when `which` is empty).
Result played(const Genre& genre, uint32_t seed, std::vector<Command> on, SteeringLog* log = nullptr) {
	Run run;
	run.recipe = newRecipe(genre, 0);
	run.seed = seed;
	run.skipIntro = true;
	run.log = log;
	std::vector<std::pair<int32_t, Command>> commands;
	for (const Command& command : on) {
		commands.push_back({kOn, command});
		Command off = command;
		off.value = 0;
		commands.push_back({kOff, off});
	}
	run.steer = steerAt(commands);
	return play(run, kBars);
}

/// Note-ons of `part` on `lane` (any lane for Lane::None) with ticks in [from, to).
std::vector<Event> notesIn(const Result& result, int32_t part, Lane lane, int32_t from, int32_t to) {
	std::vector<Event> out;
	for (const Event& e : only(result.events, EventKind::NoteOn)) {
		if (e.part == part && (lane == Lane::None || e.lane == lane) && e.tick >= from && e.tick < to) {
			out.push_back(e);
		}
	}
	return out;
}

/// Every event but those of `part` on `lane`, the pump's included: what a punch-in on that lane leaves alone.
std::vector<Event> without(const std::vector<Event>& events, int32_t part, Lane lane) {
	std::vector<Event> out;
	for (const Event& e : events) {
		const bool note = e.kind == EventKind::NoteOn || e.kind == EventKind::NoteOff;
		if (!(note && e.part == part && e.lane == lane) && e.kind != EventKind::Pump) {
			out.push_back(e);
		}
	}
	return out;
}

/// The notes from tick `from` on, with no note-offs: a roll's or a flam's last hits end a little differently.
std::vector<Event> onsFrom(const std::vector<Event>& events, int32_t from) {
	std::vector<Event> out;
	for (const Event& e : events) {
		if (e.kind == EventKind::NoteOn && e.tick >= from) {
			out.push_back(e);
		}
	}
	return out;
}

/// The commands of `kind` the last piece of `log` holds, in order.
std::vector<LogRecord> commandsLogged(const SteeringLog& log, CommandKind kind) {
	std::vector<LogRecord> out;
	log.piece(log.pieces() - 1).forEachRecord([&](const LogRecord& record) {
		if (record.kind == RecordKind::Command && record.command.kind == kind) {
			out.push_back(record);
		}
	});
	return out;
}

bool hasLane(const Genre& genre, int32_t part, Lane lane) {
	return part >= 0 && laneIndexOf(genre.parts[part], lane) >= 0;
}

} // namespace

// clang-format off
describe punch_in("Punch-ins", $ {
	it("plays the piece unchanged when a punch-in goes on and off before anything it acts on", _ {
		// Each punch-in draws nothing: around it the piece is the one with none. Here a pump, which no note
		// follows.
		for (const Genre* genre : allGenres()) {
			for (uint32_t seed : kSeeds) {
				const Result plain = played(*genre, seed, {});
				const Result pumped = played(*genre, seed, {punch(PunchIn::Pump, true)});
				expect(without(pumped.events, -1, Lane::None) == plain.events).to_be_true();
			}
		}
	});

	it("mutes one Drums lane from the step it lands on to the step it lets go on, and nothing else", _ {
		for (const Genre* genre : allGenres()) {
			const int32_t drums = drumsOf(*genre);
			if (drums < 0) {
				continue;
			}
			for (const LaneSpec& lane : genre->parts[drums].lanes) {
				for (uint32_t seed : kSeeds) {
					const Result plain = played(*genre, seed, {});
					const Result muted = played(*genre, seed, {punch(PunchIn::LaneMute, true, lane.lane)});
					// The driver drops a flagged note-on (spec §12.5): the lane plays nothing from kOn up to
					// kOff, and as it would have outside. Within Lean of either line a hit may belong to the
					// step on either side.
					auto away = [](const std::vector<Event>& notes) {
						std::vector<Event> out;
						for (const Event& e : notes) {
							if (std::abs(e.tick - stepTick(kOn)) > kMaxLean
							    && std::abs(e.tick - stepTick(kOff)) > kMaxLean) {
								out.push_back(e);
							}
						}
						return out;
					};
					expect(notesIn(muted, drums, lane.lane, stepTick(kOn) + kMaxLean + 1,
					               stepTick(kOff) - kMaxLean)
					           .size())
					    .to_equal(0U);
					auto outside = [&](const Result& r) {
						auto before = notesIn(r, drums, lane.lane, 0, stepTick(kOn));
						const auto after = notesIn(r, drums, lane.lane, stepTick(kOff), INT32_MAX);
						before.insert(before.end(), after.begin(), after.end());
						return away(before);
					};
					expect(outside(muted) == outside(plain)).to_be_true();
					expect(without(muted.events, drums, lane.lane) == without(plain.events, drums, lane.lane))
					    .to_be_true();
				}
			}
		}
	});

	it("refuses a lane mute on no lane or an FX lane, and an unknown punch-in", _ {
		Run run;
		run.recipe = acidRecipe();
		run.skipIntro = true;
		Command noValue = punch(PunchIn::Flam, true);
		noValue.value = 2;
		run.steer = steerAt({{4, punch(PunchIn::LaneMute, true)},
		                     {4, punch(PunchIn::LaneMute, true, Lane::Riser)},
		                     {4, {CommandKind::SetPunch, kNumPunchIns, 1}},
		                     {4, noValue}});
		const Result result = play(run, 2);
		expect(result.generator->punchIns().any()).to_be_false();
	});

	it("rolls the Closed hat in 32nds and the Snare (or Clap) in 16ths, from the step it lands on to the release",
	_ {
		for (const Genre* genre : allGenres()) {
			const int32_t drums = drumsOf(*genre);
			struct Roll {
				PunchIn which;
				Lane lane;
				int32_t hits;
			};
			const Lane snare = hasLane(*genre, drums, Lane::Snare) ? Lane::Snare : Lane::Clap;
			for (const Roll roll :
			     {Roll{PunchIn::HatRoll, Lane::ClosedHat, 2}, Roll{PunchIn::SnareRoll, snare, 1}}) {
				if (!hasLane(*genre, drums, roll.lane)) {
					continue;
				}
				for (uint32_t seed : kSeeds) {
					const Result plain = played(*genre, seed, {});
					const Result rolled = played(*genre, seed, {punch(roll.which, true)});
					// Every step it's on: `hits` hits evenly over the 16th from its grid line, and nothing
					// else. Up to the last step's last hit: the lane's own first hit after may lean before
					// kOff's line.
					const int32_t last = stepTick(kOff - 1) + (roll.hits - 1) * kTicksPerStep / roll.hits;
					const auto hits = notesIn(rolled, drums, roll.lane, stepTick(kOn), last + 1);
					expect(hits.size()).to_equal(static_cast<size_t>((kOff - kOn) * roll.hits));
					for (size_t i = 0; i < hits.size(); i++) {
						const int32_t step = kOn + static_cast<int32_t>(i) / roll.hits;
						const int32_t hit = static_cast<int32_t>(i) % roll.hits;
						expect(hits[i].tick).to_equal(stepTick(step) + hit * kTicksPerStep / roll.hits);
					}
					// The other lanes and parts play as they would have, and so does the lane after the
					// release.
					expect(without(rolled.events, drums, roll.lane) == without(plain.events, drums, roll.lane))
					    .to_be_true();
					expect(onsFrom(rolled.events, stepTick(kOff + 1))
					       == onsFrom(plain.events, stepTick(kOff + 1)))
					    .to_be_true();
				}
			}
		}
	});

	it("flams every Drums hit but the Kick's and doubles every Kick hit a 32nd later while on, and only then", _ {
		struct Ornament {
			PunchIn which;
			std::vector<Lane> lanes;
		};
		for (const Genre* genre : allGenres()) {
			const int32_t drums = drumsOf(*genre);
			if (drums < 0) {
				continue;
			}
			for (const Ornament& ornament :
			     {Ornament{PunchIn::Flam,
			               {Lane::Snare, Lane::Clap, Lane::Rim, Lane::ClosedHat, Lane::OpenHat, Lane::Ride,
			                Lane::Crash, Lane::Tom, Lane::Perc1, Lane::Perc2}},
			      Ornament{PunchIn::DoubleKick, {Lane::Kick}}}) {
				for (uint32_t seed : kSeeds) {
					const Result plain = played(*genre, seed, {});
					const Result punched = played(*genre, seed, {punch(ornament.which, true)});
					const auto sections = only(punched.events, EventKind::Section);
					// A hit to check: its step away from the lines where Lean could make it the next one's, and
					// from where a Section starts: a flam's grace hit has no room before that line (§8.2).
					auto checks = [&](const Event& hit) {
						const int32_t step = (hit.tick + kMaxLean) / kTicksPerStep;
						return !hit.ornament && step > kOn && step < kOff - 1
						       && std::ranges::none_of(
						           sections, [&](const Event& e) { return e.tick == stepTick(step); });
					};
					int32_t checked = 0;
					int32_t plainHits = 0;
					for (const Lane lane : ornament.lanes) {
						for (const Event& hit : notesIn(plain, drums, lane, 0, INT32_MAX)) {
							plainHits += checks(hit) ? 1 : 0;
						}
						const auto all = notesIn(punched, drums, lane, 0, INT32_MAX);
						for (size_t i = 0; i < all.size(); i++) {
							const Event& hit = all[i];
							if (!checks(hit)) {
								continue;
							}
							checked++;
							// A flam's grace hit comes just before; a double's second hit a 32nd after.
							if (ornament.which == PunchIn::Flam) {
								expect(i > 0 && all[i - 1].ornament && all[i - 1].tick < hit.tick
								       && all[i - 1].tick >= hit.tick - 6)
								    .to_be_true();
							}
							else {
								// A 32nd later, or sooner when the hit leans late in its 16th (§8.2's ratchet).
								expect(i + 1 < all.size() && all[i + 1].ornament && all[i + 1].tick > hit.tick
								       && all[i + 1].tick <= hit.tick + kRatchetSpacing[0])
								    .to_be_true();
							}
						}
						// Before it, the lane plays as it would have.
						expect(notesIn(punched, drums, lane, 0, stepTick(kOn) - kMaxLean)
						       == notesIn(plain, drums, lane, 0, stepTick(kOn) - kMaxLean))
						    .to_be_true();
					}
					expect(onsFrom(punched.events, stepTick(kOff + 1))
					       == onsFrom(plain.events, stepTick(kOff + 1)))
					    .to_be_true();
					// Every hit the piece plays there gets it, and no more hits play (a plain hit could have
					// been a ratchet's first, which the punch-in's Ornament replaces).
					expect(checked).to_equal(plainHits);
				}
			}
		}
	});

	it("sends a pump on as it lands and off as it lets go, once each", _ {
		const Result result = played(genres::technoAcid, 1, {punch(PunchIn::Pump, true)});
		const auto pumps = only(result.events, EventKind::Pump);
		expect(pumps.size()).to_equal(2U);
		if (pumps.size() == 2) {
			expect(pumps[0].tick).to_equal(stepTick(kOn));
			expect(pumps[0].pumps()).to_be_true();
			expect(pumps[1].tick).to_equal(stepTick(kOff));
			expect(pumps[1].pumps()).to_be_false();
		}
		// A second on while on sends nothing.
		Run run;
		run.recipe = acidRecipe();
		run.skipIntro = true;
		run.steer = steerAt({{4, punch(PunchIn::Pump, true)}, {8, punch(PunchIn::Pump, true)}});
		expect(only(play(run, 2).events, EventKind::Pump).size()).to_equal(1U);
	});

	it("logs each punch-in where it lands and where it lets go, a lane mute with its lane (spec §17.2)", _ {
		TestLog entries(64);
		played(genres::technoAcid, 1, {punch(PunchIn::LaneMute, true, Lane::Rim), punch(PunchIn::Flam, true)},
		       &entries.log);
		const std::vector<LogRecord> logged = commandsLogged(entries.log, CommandKind::SetPunch);
		expect(logged.size()).to_equal(4U);
		if (logged.size() == 4) {
			expect(logged[0].step).to_equal(kOn);
			expect(logged[0].command == punch(PunchIn::LaneMute, true, Lane::Rim)).to_be_true();
			expect(logged[1].command == punch(PunchIn::Flam, true)).to_be_true();
			expect(logged[2].step).to_equal(kOff);
			expect(logged[2].command == punch(PunchIn::LaneMute, false, Lane::Rim)).to_be_true();
			expect(logged[3].command == punch(PunchIn::Flam, false)).to_be_true();
		}
	});

	it("logs stock's stutter on and off where it lands, and changes nothing heard", _ {
		TestLog entries(64);
		Run run;
		run.recipe = acidRecipe();
		run.skipIntro = true;
		run.log = &entries.log;
		run.steer = steerAt({{6, {CommandKind::SetStutter, 0, 1}}, {9, {CommandKind::SetStutter, 0, 0}}});
		const Result stuttered = play(run, 2);
		run.steer = nullptr;
		run.log = nullptr;
		expect(play(run, 2).events == stuttered.events).to_be_true();
		const std::vector<LogRecord> logged = commandsLogged(entries.log, CommandKind::SetStutter);
		expect(logged.size()).to_equal(2U);
		if (logged.size() == 2) {
			expect(logged[0].step).to_equal(6);
			expect(logged[0].command.value).to_equal(1);
			expect(logged[1].step).to_equal(9);
			expect(logged[1].command.value).to_equal(0);
		}
	});

	it("starts several punch-ins on one step and lets each go on its own", _ {
		const Genre& genre = genres::technoAcid;
		const int32_t drums = drumsOf(genre);
		const Result plain = played(genre, 2, {});
		const Result both =
		    played(genre, 2, {punch(PunchIn::HatRoll, true), punch(PunchIn::LaneMute, true, Lane::Kick)});
		const auto hats = notesIn(both, drums, Lane::ClosedHat, stepTick(kOn), stepTick(kOff));
		expect(hats.size()).to_equal(static_cast<size_t>(2 * (kOff - kOn)));
		// The Kick plays none of its hits there (the driver drops a flagged note-on), the plain piece some.
		expect(notesIn(both, drums, Lane::Kick, stepTick(kOn), stepTick(kOff)).size()).to_equal(0U);
		expect(notesIn(plain, drums, Lane::Kick, stepTick(kOn), stepTick(kOff)).size()).to_be_greater_than(0U);
		expect(both.generator->punchIns().any()).to_be_false();
	});
});
// clang-format on
CPPSPEC_SPEC(punch_in)
