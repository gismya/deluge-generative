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

// The runner (spec §12.10): all long work in Generative mode, one job at a time. It has one active job and one next
// slot. Each job runs as its own one-off scheduler task, as ordinary blocking code that yields between chunks on a
// time budget, and unwinds through return codes when cancelled. Portable: the firmware plugs in the scheduler as a
// RunnerHost, the harness a fake one.

#pragma once

#include <cstdint>

namespace deluge::generative {

/// The scheduler, as the runner sees it.
class RunnerHost {
public:
	virtual ~RunnerHost() = default;
	/// Starts a one-off task, outside the caller's stack, that calls Runner::runActive().
	/// @return false if no task could be started.
	virtual bool startTask() = 0;
	/// Lets the rest of the firmware run once: audio, the tick, buttons, encoders, MIDI.
	virtual void yield() = 0;
	/// A free-running clock in the host's own units. Only differences are used, so it may wrap.
	virtual uint32_t now() = 0;
};

/// How a job ended.
enum class JobOutcome : uint8_t {
	Done,
	/// Cancelled, with everything it built freed.
	Cancelled,
	/// Stopped on an error (a card error, no memory), with everything it built freed.
	Failed,
};

/// What a running job sees of the runner.
class Chunks {
public:
	/// A chunk boundary, also called after each card read returns. Yields once the chunk has used its budget.
	/// @return whether the job is cancelled: if so it frees what it built (still calling this between chunks) and
	/// returns JobOutcome::Cancelled.
	bool boundary();
	[[nodiscard]] bool cancelled() const;

private:
	friend class Runner;
	explicit Chunks(class Runner& runner) : runner_(runner) {}
	class Runner& runner_;
};

class Job {
public:
	/// Modal jobs (Perform entry, Compose and Capture builds) leave only BACK working; live ones (preloads,
	/// freeing retired Outputs) leave the Perform page working under the busy rules (spec §12.10).
	enum class Kind : uint8_t { Modal, Live };

	explicit Job(Kind kind) : kind_(kind) {}
	virtual ~Job() = default;
	[[nodiscard]] Kind kind() const { return kind_; }

	/// The work, in the job's own task. Calls chunks.boundary() between chunks.
	virtual JobOutcome run(Chunks& chunks) = 0;
	/// In the job's task, after run() returns and before the next job starts. A job cancelled before its task ran
	/// gets this with JobOutcome::Cancelled and no run(); a job whose task couldn't start gets JobOutcome::Failed,
	/// then from the caller of submit() or from the job before it.
	virtual void finished(JobOutcome outcome) {}
	/// The job left the next slot without running: replaced by a later one, or emptied.
	virtual void dropped() {}

private:
	Kind kind_;
};

class Runner {
public:
	/// About 2 ms of work between yields: the scheduler logs a run over 3 ms as too long (spec §12.10).
	Runner(RunnerHost& host, uint32_t chunkBudget) : host_(host), chunkBudget_(chunkBudget) {}

	/// Starts `job` in its own task if the runner is free. Otherwise it waits in the next slot, replacing (and
	/// dropping) any job already waiting there: the latest wins.
	void submit(Job& job);
	/// Cancels the active job. It unwinds in its own task; the runner is free once it has.
	void cancel();
	/// Drops the job waiting in the next slot, if any.
	void clearNext();
	/// Leaving Perform: cancels the active job, empties the next slot, and runs `job` once the runner is free.
	void cancelAllThen(Job& job);

	/// Busy while a job runs or one waits (spec §12.10).
	[[nodiscard]] bool busy() const { return active_ != nullptr || next_ != nullptr; }
	[[nodiscard]] const Job* active() const { return active_; }
	[[nodiscard]] const Job* next() const { return next_; }
	[[nodiscard]] bool cancelling() const { return active_ != nullptr && cancelled_; }

	/// The task body RunnerHost::startTask() runs: the active job, then the next one is started in a task of its own.
	void runActive();

private:
	friend class Chunks;
	void start(Job& job);
	void end(JobOutcome outcome);

	RunnerHost& host_;
	uint32_t chunkBudget_;
	Job* active_ = nullptr;
	Job* next_ = nullptr;
	bool cancelled_ = false;
	uint32_t chunkStart_ = 0;
};

} // namespace deluge::generative
