// Temp paths for the harness specs: this run's own, so specs run at once in parallel worktrees (or CI jobs on one
// machine) never write or delete each other's files.

#pragma once

#include <filesystem>
#include <string>
#include <system_error>
#include <unistd.h>

namespace harness {

/// `name` under the temp directory, with this process's id on the end: no other run uses it.
inline std::filesystem::path tempPath(const std::string& name) {
	return std::filesystem::temp_directory_path() / (name + "." + std::to_string(::getpid()));
}

/// Removes a temp folder when the spec binary exits, so the per-run names don't pile up.
struct RemovedAtExit {
	std::filesystem::path path;
	~RemovedAtExit() {
		std::error_code error;
		std::filesystem::remove_all(path, error);
	}
};

} // namespace harness
