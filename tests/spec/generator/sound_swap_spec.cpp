// Sound swaps in Perform (spec §10.7, §12.9, §13.4, §13.5): every NEW SOUND, previous-sound and chosen-preset press,
// every preload's end, and every way a waiting sound swaps in or is dropped, in perform/sound_swap_lifecycle.h against
// the real core and Perform driver. The adapter is the harness's (sound_swap_piece.h, on transition_piece.h's piece);
// the firmware's is generative/firmware/sound_swaps.cpp, which the delugemu scripts check as wiring.

#include "transition_piece.h"

#include "cppspec.hpp"

#include <string>
#include <vector>

using namespace harness;

namespace {

using Press = perform::SoundPress;
using Source = perform::SoundSource;
using Outputs = std::vector<int32_t>;

constexpr int32_t kDrums = 0; ///< Techno Acid's Drums: not a synth part.
constexpr int32_t kAcid = 1;  ///< Its ACID: a synth part.
constexpr int32_t kOther = 2;

/// Techno Acid playing from its first Groove, past step 0: a press now waits for the next phrase line, not step 0's.
struct Playing : TransitionPiece {
	Playing() { runToStepLine(2); }
};

/// Plays a step past `step`'s line: a Sound event there has played.
void pastLine(Playing& t, int32_t step) {
	t.runToStepLine(step, kTicksPerStep);
}

/// `part`'s new sound preloads, and waits for its line.
void waitingSound(Playing& t, int32_t part) {
	t.sounds.press(part, false);
	t.sounds.endPreload();
}

/// `part` swaps on the first line with token 1, then the other parts' preloads spend every token up to 255.
void wrapped(Playing& t, int32_t part) {
	waitingSound(t, part);
	pastLine(t, t.phraseSteps());
	for (int32_t token = 2; token <= kMaxSoundToken; token++) {
		t.sounds.press(kOther, false);
		t.sounds.failPreload();
	}
}

/// `part`'s preload runs as a transition is confirmed, which cancels it, but it is past its last boundary and ends
/// Done while the transition is pending, after the confirm counted the waiting sounds.
void preloadedPastConfirm(Playing& t, int32_t part, TransitionStyle style) {
	t.sounds.press(part, false);
	t.runToStepLine(3, -1);
	t.lifecycle.request(style);
	t.lifecycle.load();
	t.sounds.endPreload();
}

/// Plays steps, with the pump after each, until `part`'s sound no longer waits: at most `steps`.
void playUntilSettled(Playing& t, int32_t part, int32_t steps) {
	for (int32_t s = 0; s < steps && t.sounds.waiting(part); s++) {
		t.runSteps(1);
		t.pump();
	}
}

/// The Sound event's test event for a swap on `step`'s line.
std::string swapOnLine(int32_t part, int32_t token, int32_t step) {
	return "gen_sound_swap part=" + std::to_string(part) + " line=1 token=" + std::to_string(token)
	       + " tick=" + std::to_string(stepTick(step));
}

SoundPath pathOf(const char* text) {
	SoundPath path;
	path.set(text);
	return path;
}

} // namespace

// clang-format off
describe sound_swap("Sound swaps", $ {
	it("plays Techno Acid, whose ACID is a synth part and whose Drums aren't", _ {
		expect(synthPart(genres::technoAcid, kAcid)).to_be_true();
		expect(synthPart(genres::technoAcid, kDrums)).to_be_false();
		expect(partCount(genres::technoAcid)).to_be_greater_than(kOther);
	});

	context("a press", _ {
		it("preloads the part's new sound with the next token, then waits for the line and swaps there", _ {
			Playing t;
			const int32_t phrase = t.phraseSteps();
			expect(t.sounds.press(kAcid, false) == Press::Preloading).to_be_true();
			expect(t.sounds.preloads.size()).to_equal(size_t{1});
			expect(t.sounds.preloads[0].part).to_equal(kAcid);
			expect(t.sounds.preloads[0].source == Source::Roll).to_be_true();
			expect(static_cast<int32_t>(t.sounds.preloads[0].token)).to_equal(1);
			expect(t.sounds.has("gen_sound_press part=1 source=roll token=1")).to_be_true();
			expect(t.sounds.preloading(kAcid)).to_be_true();
			expect(t.sounds.pending(kAcid)).to_be_true();
			expect(t.sounds.preloading(kOther)).to_be_false();

			t.sounds.endPreload();
			expect(t.sounds.preloading(kAcid)).to_be_false();
			expect(t.sounds.waiting(kAcid)).to_be_true();
			expect(t.sounds.waitingSounds()).to_equal(1);
			expect(t.queue.holds({CommandKind::SetSound, kAcid, 1})).to_be_true();

			t.runToStepLine(phrase, -1);
			expect(t.sounds.playing[kAcid]).to_equal(0); // decided, not played yet
			pastLine(t, phrase);
			expect(t.sounds.playing[kAcid]).to_equal(1);
			expect(t.sounds.tails == Outputs{0}).to_be_true();
			expect(t.sounds.rebound == Outputs{kAcid}).to_be_true();
			expect(t.sounds.hasPrevious(kAcid)).to_be_true();
			expect(t.sounds.pending(kAcid)).to_be_false();
			expect(t.sounds.has(swapOnLine(kAcid, 1, phrase))).to_be_true();
		});

		it("is ignored while Perform isn't entered or is being left, and for a part the genre hasn't, before Pending", _ {
			Playing t;
			t.sounds.held = [] { return true; };
			t.sounds.entered = false;
			expect(t.sounds.press(kAcid, false) == Press::Ignored).to_be_true();
			t.sounds.entered = true;
			t.sounds.left = true;
			expect(t.sounds.press(kAcid, false) == Press::Ignored).to_be_true();
			t.sounds.left = false;
			expect(t.sounds.press(-1, false) == Press::Ignored).to_be_true();
			expect(t.sounds.press(partCount(genres::technoAcid), false) == Press::Ignored).to_be_true();
			expect(t.sounds.press(kAcid, false) == Press::Pending).to_be_true();
			expect(t.sounds.preloads.empty()).to_be_true();
			expect(t.sounds.events.empty()).to_be_true();
		});

		it("is refused with CHANGE PENDING while a transition is pending, before it would cancel", _ {
			Playing t;
			t.sounds.press(kAcid, false);
			t.confirmAt(3, TransitionStyle::BuildIntoDrop); // cancels the preload, which unwinds
			expect(t.sounds.cancelledJobs).to_equal(1);
			expect(t.sounds.press(kAcid, false) == Press::Pending).to_be_true(); // still preloading
			expect(t.sounds.cancelledJobs).to_equal(1);
			t.sounds.endPreload(); // past its last boundary: retired unplayed (#316)
			expect(t.sounds.press(kAcid, false) == Press::Pending).to_be_true();
			expect(t.sounds.waiting(kAcid)).to_be_false();
			expect(t.sounds.unplayed == Outputs{1}).to_be_true();
		});

		it("cancels the part's preload when pressed again", _ {
			Playing t;
			t.sounds.press(kAcid, false);
			t.sounds.jobAhead = true; // the cancel goes before the runner's Busy
			expect(t.sounds.press(kAcid, true) == Press::Cancelled).to_be_true(); // and before NoPrevious
			expect(t.sounds.cancelledJobs).to_equal(1);
			expect(t.sounds.has("gen_sound_cancel part=1 preload")).to_be_true();
			expect(t.sounds.preloads.size()).to_equal(size_t{1});
			t.sounds.failPreload();
			expect(t.sounds.pending(kAcid)).to_be_false();
		});

		it("cancels the part's waiting sound when pressed again, in the core too, and its Output goes unplayed", _ {
			Playing t;
			const int32_t phrase = t.phraseSteps();
			waitingSound(t, kAcid);
			t.decideOne();
			expect(t.sounds.press(kAcid, false) == Press::Cancelled).to_be_true();
			expect(t.queue.holds({CommandKind::SetSound, kAcid, kNoSound})).to_be_true();
			expect(t.sounds.unplayed == Outputs{1}).to_be_true();
			expect(t.sounds.has("gen_sound_cancel part=1 waiting")).to_be_true();
			expect(t.sounds.pending(kAcid)).to_be_false();
			pastLine(t, phrase);
			expect(t.out->soundLanded(kAcid)).to_equal(-1);
			expect(t.sounds.playing[kAcid]).to_equal(0);
			expect(t.sounds.tails.empty()).to_be_true();
		});

		it("is too late once the token has landed: the swap plays on its line", _ {
			Playing t;
			const int32_t phrase = t.phraseSteps();
			waitingSound(t, kAcid);
			t.runToStepLine(phrase, -1);
			expect(t.out->soundLanded(kAcid)).to_equal(1);
			expect(t.sounds.press(kAcid, false) == Press::Ignored).to_be_true();
			expect(t.sounds.has("gen_sound_cancel part=1 too_late")).to_be_true();
			expect(t.queue.size()).to_equal(0);
			pastLine(t, phrase);
			expect(t.sounds.playing[kAcid]).to_equal(1);
		});

		it("is refused with QUEUE FULL when a waiting sound's cancel has no room, and it swaps on its line", _ {
			Playing t;
			const int32_t phrase = t.phraseSteps();
			waitingSound(t, kAcid);
			t.decideOne();
			t.fillQueue();
			expect(t.sounds.press(kAcid, false) == Press::QueueFull).to_be_true();
			expect(t.sounds.has("gen_sound_cancel part=1 queue_full")).to_be_true();
			expect(t.sounds.waiting(kAcid)).to_be_true();
			t.decideOne();
			t.sounds.queueHeld = true; // the test port's hold: refused with room in the queue
			expect(t.sounds.press(kAcid, false) == Press::QueueFull).to_be_true();
			expect(t.queue.size()).to_equal(0);
			pastLine(t, phrase);
			expect(t.sounds.playing[kAcid]).to_equal(1);
			expect(t.sounds.unplayed.empty()).to_be_true();
		});

		it("has no previous sound to go back to before the part's first swap, before Busy", _ {
			Playing t;
			t.sounds.jobAhead = true;
			expect(t.sounds.press(kAcid, true) == Press::NoPrevious).to_be_true();
			expect(t.sounds.preloads.empty()).to_be_true();
		});

		it("is refused with BUSY while the runner has a job, its own preload's included", _ {
			Playing t;
			t.sounds.jobAhead = true;
			expect(t.sounds.press(kAcid, false) == Press::Busy).to_be_true();
			t.sounds.jobAhead = false;
			t.sounds.press(kAcid, false);
			expect(t.sounds.press(kOther, false) == Press::Busy).to_be_true();
			expect(t.sounds.preloads.size()).to_equal(size_t{1});
		});

		it("is refused with BUSY when the sound share can't take every Output its re-rolls may retire", _ {
			Playing t;
			t.sounds.freeing = perform::kSoundRetirements;
			expect(t.sounds.press(kAcid, false) == Press::Busy).to_be_true();
			t.sounds.freeing = perform::kSoundRetirements - 1;
			waitingSound(t, kAcid); // its Output may retire too: no place left for another
			expect(t.sounds.press(kOther, false) == Press::Busy).to_be_true();
			t.sounds.freeing--;
			expect(t.sounds.press(kOther, false) == Press::Preloading).to_be_true();
		});

		it("is refused with BUSY when the runner doesn't take the preload, and spends the token", _ {
			Playing t;
			t.sounds.submitRefused = true;
			expect(t.sounds.press(kAcid, false) == Press::Busy).to_be_true();
			expect(t.sounds.events.empty()).to_be_true();
			t.sounds.submitRefused = false;
			expect(t.sounds.press(kAcid, false) == Press::Preloading).to_be_true();
			expect(static_cast<int32_t>(t.sounds.preloads.back().token)).to_equal(2);
		});
	});

	context("a preload's end", _ {
		it("swaps in at once while stopped, with no line to wait for", _ {
			Playing t;
			t.stop();
			t.sounds.press(kAcid, false);
			t.sounds.endPreload();
			expect(t.sounds.playing[kAcid]).to_equal(1);
			expect(t.sounds.tails == Outputs{0}).to_be_true();
			expect(t.sounds.rebound.empty()).to_be_true(); // the next start binds every part
			expect(t.sounds.hasPrevious(kAcid)).to_be_true();
			expect(t.sounds.waiting(kAcid)).to_be_false();
			expect(t.queue.size()).to_equal(0);
			expect(t.sounds.has("gen_sound_swap part=1 stopped token=1")).to_be_true();
		});

		it("drops its Output with QUEUE FULL when the queue has no room for its swap", _ {
			Playing t;
			t.sounds.press(kAcid, false);
			t.fillQueue();
			t.sounds.endPreload();
			expect(t.sounds.waiting(kAcid)).to_be_false();
			expect(t.sounds.unplayed == Outputs{1}).to_be_true();
			expect(t.sounds.queueFullPopups).to_equal(1);
		});

		it("does nothing while Perform is being left: its Output goes with Perform's song", _ {
			Playing t;
			t.sounds.press(kAcid, false);
			t.sounds.left = true;
			t.sounds.endPreload();
			expect(t.sounds.waiting(kAcid)).to_be_false();
			expect(t.sounds.unplayed.empty()).to_be_true();
			expect(t.sounds.playing[kAcid]).to_equal(0);
			expect(t.queue.size()).to_equal(0);
		});

		it("changes nothing when it was cancelled or failed: the part can be pressed again", _ {
			Playing t;
			t.sounds.press(kAcid, false);
			t.sounds.failPreload();
			expect(t.sounds.pending(kAcid)).to_be_false();
			expect(t.queue.size()).to_equal(0);
			expect(t.sounds.press(kAcid, false) == Press::Preloading).to_be_true();
		});
	});

	context("a waiting sound", _ {
		it("swaps only on a Sound event that carries its token", _ {
			Playing t;
			waitingSound(t, kAcid);
			t.sounds.swap(Event::sound(0, kAcid, 2));
			t.sounds.swap(Event::sound(0, kMaxParts, 1));
			expect(t.sounds.waiting(kAcid)).to_be_true();
			expect(t.sounds.tails.empty()).to_be_true();
			t.sounds.swap(Event::sound(0, kAcid, 1));
			expect(t.sounds.playing[kAcid]).to_equal(1);
		});

		it("is dropped once the core has dropped it (a BACK tap), not before the core has taken it", _ {
			Playing t;
			waitingSound(t, kAcid);
			t.pump(); // the core hasn't taken it yet
			expect(t.sounds.waiting(kAcid)).to_be_true();
			t.decideOne();
			t.pump();
			expect(t.sounds.waiting(kAcid)).to_be_true();
			t.pushAndDecide({CommandKind::CancelNewest, 0, 0});
			expect(t.out->soundWaiting(kAcid)).to_equal(-1);
			t.pump();
			expect(t.sounds.waiting(kAcid)).to_be_false();
			expect(t.sounds.unplayed == Outputs{1}).to_be_true();
			expect(t.sounds.has("gen_sound_cancel part=1 dropped")).to_be_true();
		});

		it("is dropped at a stop", _ {
			Playing t;
			waitingSound(t, kAcid);
			t.decideOne();
			t.stop();
			expect(t.sounds.waiting(kAcid)).to_be_false();
			expect(t.sounds.unplayed == Outputs{1}).to_be_true();
			expect(t.sounds.has("gen_sound_cancel part=1 stopped")).to_be_true();
		});

		it("is dropped at a transition's confirm, with a cancel in the core, and a preload running is cancelled", _ {
			Playing t;
			waitingSound(t, kAcid);
			t.sounds.press(kOther, false);
			t.runToStepLine(3, -1);
			t.lifecycle.request(TransitionStyle::BuildIntoDrop);
			expect(t.lifecycle.soundDrops).to_equal(1);
			expect(t.sounds.cancelledJobs).to_equal(1);
			expect(t.sounds.has("gen_sound_cancel preload transition")).to_be_true();
			expect(t.sounds.has("gen_sound_cancel part=1 transition")).to_be_true();
			expect(t.queue.holds({CommandKind::SetSound, kAcid, kNoSound})).to_be_true();
			expect(t.sounds.waitingSounds()).to_equal(0);
			expect(t.sounds.unplayed == Outputs{1}).to_be_true();
			t.decideOne();
			expect(t.out->soundWaiting(kAcid)).to_equal(-1);
		});

		it("is dropped by a confirm between its landing and its Sound event: the part plays on (decision 2, #261)", _ {
			// The core has logged the SetSound, which never plays: M4's steering log settles this.
			Playing t;
			const int32_t phrase = t.phraseSteps();
			waitingSound(t, kAcid);
			t.runToStepLine(phrase, -1);
			expect(t.out->soundLanded(kAcid)).to_equal(1);
			t.lifecycle.request(TransitionStyle::BuildIntoDrop);
			pastLine(t, phrase);
			expect(t.out->soundLanded(kAcid)).to_equal(1);
			expect(t.sounds.playing[kAcid]).to_equal(0);
			expect(t.sounds.unplayed == Outputs{1}).to_be_true();
		});
	});

	context("the tokens", _ {
		it("skip the token that last landed on the part when they wrap: a press still cancels it", _ {
			Playing t;
			wrapped(t, kAcid);
			expect(t.out->soundLanded(kAcid)).to_equal(1);
			expect(static_cast<int32_t>(t.sounds.preloads.back().token)).to_equal(kMaxSoundToken);
			waitingSound(t, kAcid);
			expect(static_cast<int32_t>(t.sounds.preloads.back().token)).to_equal(2);
			t.decideOne();
			expect(t.sounds.press(kAcid, false) == Press::Cancelled).to_be_true();
			expect(t.sounds.unplayed == Outputs{2}).to_be_true();
		});

		it("skip the token that last landed on the part when they wrap: the pump sees the core drop it", _ {
			Playing t;
			wrapped(t, kAcid);
			expect(t.out->soundLanded(kAcid)).to_equal(1);
			expect(static_cast<int32_t>(t.sounds.preloads.back().token)).to_equal(kMaxSoundToken);
			waitingSound(t, kAcid);
			t.decideOne();
			t.pushAndDecide({CommandKind::CancelNewest, 0, 0});
			t.pump();
			expect(t.sounds.waiting(kAcid)).to_be_false();
			expect(t.sounds.press(kAcid, false) == Press::Preloading).to_be_true();
		});

		it("skip the landed 255 to 1 when the counter stands at 254", _ {
			Playing t;
			for (int32_t token = 1; token < kMaxSoundToken; token++) {
				t.sounds.press(kOther, false);
				t.sounds.failPreload();
			}
			waitingSound(t, kAcid);
			expect(static_cast<int32_t>(t.sounds.preloads.back().token)).to_equal(kMaxSoundToken);
			pastLine(t, t.phraseSteps());
			expect(t.out->soundLanded(kAcid)).to_equal(kMaxSoundToken);
			for (int32_t token = 1; token < kMaxSoundToken; token++) {
				t.sounds.press(kOther, false);
				t.sounds.failPreload();
			}
			expect(static_cast<int32_t>(t.sounds.preloads.back().token)).to_equal(kMaxSoundToken - 1);
			t.sounds.press(kAcid, false);
			expect(static_cast<int32_t>(t.sounds.preloads.back().token)).to_equal(1);
		});

		it("go on from 1 for a part with another token landed", _ {
			Playing t;
			wrapped(t, kAcid);
			expect(t.out->soundLanded(kAcid)).to_equal(1);
			expect(static_cast<int32_t>(t.sounds.preloads.back().token)).to_equal(kMaxSoundToken);
			waitingSound(t, kOther);
			expect(static_cast<int32_t>(t.sounds.preloads.back().token)).to_equal(1);
		});
	});

	// The confirm's cancelAll() cancelled the preload, but it was past its last boundary and ended Done (#316). Sound
	// actions are refused while a transition is pending (spec §12.9), so its Output is retired unplayed: nothing waits
	// uncounted by the confirm, and the part's sound doesn't change, whatever the transition's style.
	context("a preload that ends Done after a confirm cancelled it", _ {
		it("is retired unplayed, with nothing queued for the part", _ {
			Playing t;
			t.sounds.press(kAcid, false);
			t.runToStepLine(3, -1);
			t.lifecycle.request(TransitionStyle::BuildIntoDrop);
			expect(t.sounds.cancelledJobs).to_equal(1);
			t.lifecycle.load();
			t.sounds.endPreload();
			expect(t.lifecycle.pending()).to_be_true();
			expect(t.sounds.pending(kAcid)).to_be_false();
			expect(t.sounds.waitingSounds()).to_equal(0);
			expect(t.queue.holds({CommandKind::SetSound, kAcid, 1})).to_be_false();
			expect(t.sounds.incoming[kAcid]).to_equal(0);
			expect(t.sounds.unplayed == Outputs{1}).to_be_true();
			expect(t.sounds.has("gen_sound_cancel part=1 transition")).to_be_true();
		});

		it("leaves the part's sound alone through the transition, in each style", _ {
			for (const TransitionStyle style : {TransitionStyle::BuildIntoDrop, TransitionStyle::Fade, TransitionStyle::Cut}) {
				Playing t;
				const int32_t phrase = t.phraseSteps();
				preloadedPastConfirm(t, kAcid, style);
				pastLine(t, phrase);
				for (int32_t s = 0; s < 2 * phrase; s++) {
					t.runSteps(1);
					t.pump();
				}
				expect(t.sounds.playing[kAcid]).to_equal(0);
				expect(t.sounds.tails.empty()).to_be_true();
				expect(t.sounds.unplayed == Outputs{1}).to_be_true();
				expect(t.sounds.has(swapOnLine(kAcid, 1, phrase))).to_be_false();
				expect(t.sounds.has("gen_sound_cancel part=1 dropped")).to_be_false();
			}
		});
	});

	context("the previous sound", _ {
		it("goes back to the previous sound, and on to the one before", _ {
			Playing t;
			const int32_t phrase = t.phraseSteps();
			waitingSound(t, kAcid);
			pastLine(t, phrase);
			expect(t.sounds.press(kAcid, true) == Press::Preloading).to_be_true();
			expect(t.sounds.preloads.back().source == Source::Previous).to_be_true();
			expect(t.sounds.has("gen_sound_press part=1 source=previous token=2")).to_be_true();
			t.sounds.endPreload();
			pastLine(t, 2 * phrase);
			expect(t.sounds.playing[kAcid]).to_equal(2);
			expect(t.sounds.tails == (Outputs{0, 1})).to_be_true();
			expect(t.sounds.press(kAcid, true) == Press::Preloading).to_be_true();
			expect(static_cast<int32_t>(t.sounds.preloads.back().token)).to_equal(3);
		});

		it("is forgotten when a transition's new roster takes over, and when Perform's song goes", _ {
			Playing t;
			const int32_t phrase = t.phraseSteps();
			waitingSound(t, kAcid);
			pastLine(t, phrase);
			expect(t.sounds.hasPrevious(kAcid)).to_be_true();
			t.confirm(TransitionStyle::Cut);
			t.stop(); // commits the transition stopped
			expect(t.sounds.hasPrevious(kAcid)).to_be_false();

			Playing other;
			other.stop();
			waitingSound(other, kAcid); // stopped: swapped in at once
			other.sounds.forget();
			expect(other.sounds.hasPrevious(kAcid)).to_be_false();
		});
	});

	context("a chosen preset", _ {
		it("preloads as a re-roll's, with its path, on a synth part with nothing pending", _ {
			Playing t;
			const SoundPath path = pathOf("SYNTHS/ACID.XML");
			expect(t.sounds.canChoose(kAcid, path) == Press::Preloading).to_be_true();
			expect(t.sounds.choose(kAcid, path) == Press::Preloading).to_be_true();
			expect(t.sounds.preloads.back().source == Source::Chosen).to_be_true();
			expect(t.sounds.preloads.back().chosen).to_equal(std::string("SYNTHS/ACID.XML"));
			expect(t.sounds.has("gen_sound_press part=1 source=chosen token=1")).to_be_true();
			expect(t.sounds.canStart(kAcid) == Press::Ignored).to_be_true(); // pending
			t.sounds.endPreload();
			expect(t.sounds.waiting(kAcid)).to_be_true();
		});

		it("is refused as canChoose() says: Ignored, then Pending, then Busy", _ {
			Playing t;
			const SoundPath path = pathOf("SYNTHS/ACID.XML");
			expect(t.sounds.choose(kAcid, SoundPath{}) == Press::Ignored).to_be_true();
			expect(t.sounds.choose(kDrums, path) == Press::Ignored).to_be_true();
			t.sounds.jobAhead = true;
			expect(t.sounds.choose(kAcid, path) == Press::Busy).to_be_true();
			t.sounds.held = [] { return true; };
			expect(t.sounds.choose(kAcid, path) == Press::Pending).to_be_true();
			expect(t.sounds.canStart(kDrums) == Press::Pending).to_be_true();
			expect(t.sounds.preloads.empty()).to_be_true();
		});
	});
});

CPPSPEC_SPEC(sound_swap)
