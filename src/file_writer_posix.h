// src/file_writer_posix.h (private; macOS and Linux)
#pragma once

#include <filesystem>

#include <sys/types.h>

namespace dvv::platform::detail {
// The mode the temporary file is created with, before the umask: an existing target's permission
// bits, so the new contents are never readable by more users than the target's; 0666 for a new
// target; 0600 when an existing target's mode cannot be read.
[[nodiscard]] mode_t temporaryMode(const std::filesystem::path& target, bool target_exists);
}  // namespace dvv::platform::detail
