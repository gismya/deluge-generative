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

#include "generative/runner/runner.h"

namespace deluge::generative {

bool Chunks::boundary() {
	if (runner_.host_.now() - runner_.chunkStart_ >= runner_.chunkBudget_) {
		runner_.host_.yield();
		runner_.chunkStart_ = runner_.host_.now();
	}
	return cancelled();
}

bool Chunks::cancelled() const {
	return runner_.cancelled_;
}

void Chunks::commit() {
	runner_.committed_ = true;
}

void Runner::submit(Job& job) {
	if (&job == active_ || &job == next_) {
		return;
	}
	if (active_ == nullptr) {
		start(job);
		return;
	}
	clearNext();
	next_ = &job;
}

void Runner::start(Job& job) {
	active_ = &job;
	cancelled_ = false;
	committed_ = false;
	if (!host_.startTask()) {
		end(JobOutcome::Failed); // no task to run it in: it built nothing
	}
}

void Runner::end(JobOutcome outcome) {
	active_->finished(outcome);

	// The runner is free only now, with the job's teardown done: the next job starts in a task of its own.
	active_ = nullptr;
	cancelled_ = false;
	if (next_ != nullptr) {
		Job* waiting = next_;
		next_ = nullptr;
		start(*waiting);
	}
}

bool Runner::cancel() {
	if (active_ != nullptr && !committed_ && active_->cancellable()) {
		cancelled_ = true;
	}
	return cancelling();
}

void Runner::clearNext() {
	Job* waiting = next_;
	next_ = nullptr;
	if (waiting != nullptr) {
		waiting->dropped();
	}
}

void Runner::cancelAllThen(Job& job) {
	clearNext();
	cancel();
	submit(job);
}

void Runner::runActive() {
	Job* job = active_;
	if (job == nullptr) {
		return;
	}
	// A job cancelled before its task came round has built nothing, so it never runs.
	JobOutcome outcome = JobOutcome::Cancelled;
	if (!cancelled_) {
		Chunks chunks(*this);
		chunkStart_ = host_.now();
		outcome = job->run(chunks);
	}
	end(outcome);
}

} // namespace deluge::generative
