// src/file_writer_posix.h (private; macOS and Linux)
#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <span>

#include <sys/stat.h>
#include <sys/types.h>

#include "file_writer_detail.h"

namespace dvv::platform::detail {
// The mode the temporary file is created with, before the umask: an existing target's permission
// bits, so the new contents are never readable by more users than the target's; 0666 for a new
// target; 0600 when an existing target's mode cannot be read.
[[nodiscard]] mode_t temporaryMode(const std::filesystem::path& target, bool target_exists);

// Sets an open file's mode: fchmod, or a fake in the tests. 0 on success, else -1 with errno set.
using ModeSetter = int (*)(int file, mode_t mode);

// writeAndReplace with the call that gives an existing target's mode back to the temporary file
// when the umask removed bits. It is called only when the created mode differs; EPERM, ENOTSUP,
// EOPNOTSUPP and EINVAL from it mean the file system has no POSIX modes (vfat, exFAT, some network
// mounts) and the mount decides, so the replace goes on.
[[nodiscard]] std::expected<void, WriteFailure> writeAndReplaceWith(
    const std::filesystem::path& target, const std::filesystem::path& temporary, bool target_exists,
    std::span<const std::byte> bytes, const ReplaceFileOptions& options, ModeSetter set_mode);
}  // namespace dvv::platform::detail
