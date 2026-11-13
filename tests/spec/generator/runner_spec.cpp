// The runner (spec §12.10): one active job and one next slot, each job in its own task, yielding on a time budget,
// the busy flag, and cancel.

#include "generative/runner/runner.h"

#include "cppspec.hpp"

#include <functional>
#include <string>
#include <vector>

using namespace deluge::generative;

namespace {

constexpr uint32_t kBudget = 2000;

/// A scheduler with tasks the spec runs by hand. Work costs whatever the job says it spent; a yield runs `onYield`,
/// standing in for the buttons task and everything else the firmware runs between chunks.
struct FakeHost final : RunnerHost {
	int32_t tasksStarted = 0;
	int32_t tasksPending = 0;
	int32_t yields = 0;
	uint32_t clock = 0;
	bool taskListFull = false;
	std::function<void()> onYield;

	bool startTask() override {
		if (taskListFull) {
			return false;
		}
		tasksStarted++;
		tasksPending++;
		return true;
	}
	void yield() override {
		yields++;
		if (onYield) {
			onYield();
		}
	}
	uint32_t now() override { return clock; }

	/// Runs scheduled tasks until none are left.
	void runTasks(Runner& runner) {
		while (tasksPending > 0) {
			tasksPending--;
			runner.runActive();
		}
	}
};

/// A job of `chunks` chunks of `cost` each, recording what happened to it into a shared log.
struct StepJob final : Job {
	std::string name;
	std::vector<std::string>& log;
	FakeHost& host;
	int32_t chunks;
	uint32_t cost;
	int32_t chunksRun = 0;
	int32_t chunksFreed = 0;
	int32_t commitAfter = -1; ///< Commits (past the point of no return) after this many chunks; -1 never.
	bool stoppable = true;    ///< False for a job that can never be cancelled, such as a teardown.

	StepJob(std::string name, std::vector<std::string>& log, FakeHost& host, int32_t chunks = 3, uint32_t cost = 500,
	        Kind kind = Kind::Modal)
	    : Job(kind), name(std::move(name)), log(log), host(host), chunks(chunks), cost(cost) {}

	JobOutcome run(Chunks& c) override {
		log.push_back(name + " run");
		for (int32_t i = 0; i < chunks; i++) {
			host.clock += cost;
			chunksRun++;
			if (chunksRun == commitAfter) {
				c.commit();
			}
			if (c.boundary()) {
				// Free what was built, also in chunks.
				for (int32_t j = 0; j < chunksRun; j++) {
					chunksFreed++;
					c.boundary();
				}
				return JobOutcome::Cancelled;
			}
		}
		return JobOutcome::Done;
	}
	void finished(JobOutcome outcome) override {
		log.push_back(name
		              + (outcome == JobOutcome::Done        ? " done"
		                 : outcome == JobOutcome::Cancelled ? " cancelled"
		                                                    : " failed"));
	}
	void dropped() override { log.push_back(name + " dropped"); }
	[[nodiscard]] bool cancellable() const override { return stoppable; }
};

} // namespace

// clang-format off
describe runner("The runner", $ {
	it("runs a job in a task of its own, never inside submit", _ {
		FakeHost host;
		Runner runner(host, kBudget);
		std::vector<std::string> log;
		StepJob job("a", log, host);

		runner.submit(job);
		expect(log.empty()).to_be_true();
		expect(host.tasksStarted).to_equal(1);
		expect(runner.busy()).to_be_true();

		host.runTasks(runner);
		expect(log == std::vector<std::string>{"a run", "a done"}).to_be_true();
		expect(runner.busy()).to_be_false();
	});

	it("yields at a chunk boundary once the chunk has used its budget, not before", _ {
		FakeHost host;
		Runner runner(host, kBudget);
		std::vector<std::string> log;
		StepJob job("a", log, host, 6, 500); // 3000 of work in six boundaries

		runner.submit(job);
		host.runTasks(runner);
		expect(host.yields).to_equal(1); // at the fourth boundary, 2000 in; the last 1000 fits in the next chunk
	});

	it("yields after every chunk that outruns the budget on its own, as a card read does", _ {
		FakeHost host;
		Runner runner(host, kBudget);
		std::vector<std::string> log;
		StepJob job("a", log, host, 3, 10000);

		runner.submit(job);
		host.runTasks(runner);
		expect(host.yields).to_equal(3);
	});

	it("holds a job submitted while one runs in the next slot, and starts it in its own task once the first ends", _ {
		FakeHost host;
		Runner runner(host, kBudget);
		std::vector<std::string> log;
		StepJob a("a", log, host);
		StepJob b("b", log, host);

		runner.submit(a);
		runner.submit(b);
		expect(host.tasksStarted).to_equal(1);
		expect(runner.next() == &b).to_be_true();

		host.tasksPending--;
		runner.runActive(); // a's task
		expect(log == std::vector<std::string>{"a run", "a done"}).to_be_true();
		expect(runner.active() == &b).to_be_true();
		expect(runner.busy()).to_be_true();
		expect(host.tasksStarted).to_equal(2);

		host.runTasks(runner);
		expect(log == std::vector<std::string>{"a run", "a done", "b run", "b done"}).to_be_true();
		expect(runner.busy()).to_be_false();
	});

	it("keeps only the latest waiting job: a later submit drops the one in the next slot", _ {
		FakeHost host;
		Runner runner(host, kBudget);
		std::vector<std::string> log;
		StepJob a("a", log, host);
		StepJob b("b", log, host);
		StepJob c("c", log, host);

		runner.submit(a);
		runner.submit(b);
		runner.submit(c);
		expect(log == std::vector<std::string>{"b dropped"}).to_be_true();

		host.runTasks(runner);
		expect(log == std::vector<std::string>{"b dropped", "a run", "a done", "c run", "c done"}).to_be_true();
	});

	it("cancels at the next chunk boundary, and stays busy until the job has freed what it built", _ {
		FakeHost host;
		Runner runner(host, kBudget);
		std::vector<std::string> log;
		StepJob job("a", log, host, 10, 2000); // every chunk fills the budget, so every boundary yields
		bool busyWhileFreeing = true;
		host.onYield = [&] {
			if (job.chunksRun == 2 && !runner.cancelling()) {
				runner.cancel(); // BACK, pressed between chunks
			}
			if (job.chunksFreed > 0) {
				busyWhileFreeing = busyWhileFreeing && runner.busy();
			}
		};

		runner.submit(job);
		host.runTasks(runner);
		expect(job.chunksRun).to_equal(2);
		expect(job.chunksFreed).to_equal(2);
		expect(busyWhileFreeing).to_be_true();
		expect(log == std::vector<std::string>{"a run", "a cancelled"}).to_be_true();
		expect(runner.busy()).to_be_false();
	});

	it("starts the next job only once a cancelled one has unwound", _ {
		FakeHost host;
		Runner runner(host, kBudget);
		std::vector<std::string> log;
		StepJob a("a", log, host, 10, 2000);
		StepJob b("b", log, host);
		host.onYield = [&] {
			if (a.chunksRun == 1 && !runner.cancelling() && runner.active() == &a) {
				runner.cancel();
				runner.submit(b); // a transition request during Preloading: cancel, then the next slot
			}
		};

		runner.submit(a);
		host.runTasks(runner);
		expect(log == std::vector<std::string>{"a run", "a cancelled", "b run", "b done"}).to_be_true();
		expect(b.chunksFreed).to_equal(0); // the cancel was a's, not b's
	});

	it("refuses to cancel a job past its point of no return", _ {
		FakeHost host;
		Runner runner(host, kBudget);
		std::vector<std::string> log;
		StepJob job("a", log, host, 4, 2000);
		job.commitAfter = 2;
		bool refused = false;
		host.onYield = [&] {
			if (job.chunksRun == 2) {
				refused = !runner.cancel();
			}
		};

		runner.submit(job);
		host.runTasks(runner);
		expect(refused).to_be_true();
		expect(job.chunksRun).to_equal(4);
		expect(log == std::vector<std::string>{"a run", "a done"}).to_be_true();
	});

	it("starts each run of a job cancellable again, however the last run ended", _ {
		FakeHost host;
		Runner runner(host, kBudget);
		std::vector<std::string> log;
		StepJob job("a", log, host, 4, 2000);
		job.commitAfter = 2;

		runner.submit(job);
		host.runTasks(runner); // committed, then done
		runner.submit(job);
		expect(runner.cancel()).to_be_true(); // BACK before the second run's task has started
		host.runTasks(runner);
		expect(log == std::vector<std::string>{"a run", "a done", "a cancelled"}).to_be_true();
	});

	it("never cancels a job that can't be stopped", _ {
		FakeHost host;
		Runner runner(host, kBudget);
		std::vector<std::string> log;
		StepJob teardown("teardown", log, host);
		teardown.stoppable = false;

		runner.submit(teardown);
		expect(runner.cancel()).to_be_false();
		host.runTasks(runner);
		expect(log == std::vector<std::string>{"teardown run", "teardown done"}).to_be_true();
	});

	it("never runs a job cancelled before its task started, but still tells it", _ {
		FakeHost host;
		Runner runner(host, kBudget);
		std::vector<std::string> log;
		StepJob job("a", log, host);

		runner.submit(job);
		runner.cancel();
		expect(runner.busy()).to_be_true();
		host.runTasks(runner);
		expect(log == std::vector<std::string>{"a cancelled"}).to_be_true();
		expect(runner.busy()).to_be_false();
	});

	it("reports a submit it ignores: the job is already active or waiting", _ {
		FakeHost host;
		Runner runner(host, kBudget);
		std::vector<std::string> log;
		StepJob a("a", log, host);
		StepJob b("b", log, host);

		expect(runner.submit(a)).to_be_true();
		expect(runner.submit(a)).to_be_false();
		expect(runner.submit(b)).to_be_true();
		expect(runner.submit(b)).to_be_false();
		expect(runner.cancelAllThen(b)).to_be_false(); // b waits already: nothing changes, a isn't cancelled
		host.runTasks(runner);
		expect(log == std::vector<std::string>{"a run", "a done", "b run", "b done"}).to_be_true();
	});

	it("empties the next slot on request, dropping the waiting job", _ {
		FakeHost host;
		Runner runner(host, kBudget);
		std::vector<std::string> log;
		StepJob a("a", log, host);
		StepJob b("b", log, host);

		runner.submit(a);
		runner.submit(b);
		runner.clearNext();
		expect(log == std::vector<std::string>{"b dropped"}).to_be_true();
		host.runTasks(runner);
		expect(log == std::vector<std::string>{"b dropped", "a run", "a done"}).to_be_true();
	});

	it("on leaving, cancels the active job, empties the next slot and runs the teardown once the job has unwound", _ {
		FakeHost host;
		Runner runner(host, kBudget);
		std::vector<std::string> log;
		StepJob preload("preload", log, host, 10, 2000, Job::Kind::Live);
		StepJob waiting("waiting", log, host);
		StepJob teardown("teardown", log, host);
		host.onYield = [&] {
			if (preload.chunksRun == 3 && runner.active() == &preload && !runner.cancelling()) {
				runner.submit(waiting);
				runner.cancelAllThen(teardown); // LEAVE, during the preload
			}
		};

		runner.submit(preload);
		host.runTasks(runner);
		expect(log == std::vector<std::string>{"preload run", "waiting dropped", "preload cancelled", "teardown run",
		                                       "teardown done"})
		    .to_be_true();
		expect(preload.chunksFreed).to_equal(preload.chunksRun);
		expect(runner.busy()).to_be_false();
	});

	it("fails a job whose task can't be started, and moves on to the next", _ {
		FakeHost host;
		Runner runner(host, kBudget);
		std::vector<std::string> log;
		StepJob a("a", log, host);
		StepJob b("b", log, host);
		StepJob c("c", log, host);

		host.taskListFull = true;
		runner.submit(a);
		expect(log == std::vector<std::string>{"a failed"}).to_be_true();
		expect(runner.busy()).to_be_false();

		host.taskListFull = false;
		runner.submit(b);
		runner.submit(c);
		host.taskListFull = true;
		host.runTasks(runner); // b runs; c's task can't start
		expect(log == std::vector<std::string>{"a failed", "b run", "b done", "c failed"}).to_be_true();
		expect(runner.busy()).to_be_false();
	});

	it("on leaving while free, runs the teardown at once", _ {
		FakeHost host;
		Runner runner(host, kBudget);
		std::vector<std::string> log;
		StepJob teardown("teardown", log, host);

		runner.cancelAllThen(teardown);
		host.runTasks(runner);
		expect(log == std::vector<std::string>{"teardown run", "teardown done"}).to_be_true();
	});
});

CPPSPEC_SPEC(runner)
