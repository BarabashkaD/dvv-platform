// src/file_writer_win32.h (private; Windows)
#pragma once

namespace dvv::platform::detail {
// Whether a failed replace step may be tried again (`code` is a GetLastError() value); these codes
// leave both files intact. Always: sharing violation (32), lock violation (33), access denied (5),
// ERROR_UNABLE_TO_REMOVE_REPLACED (1175). A move to a new target: ERROR_ALREADY_EXISTS (183) and
// ERROR_FILE_EXISTS (80), another writer created it first. ReplaceFileW on an existing target:
// ERROR_FILE_NOT_FOUND (2), another writer's replace has briefly moved it away.
[[nodiscard]] bool isRetryableReplaceError(unsigned long code, bool new_target) noexcept;
}  // namespace dvv::platform::detail
