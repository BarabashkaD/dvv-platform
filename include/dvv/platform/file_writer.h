#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

#include "dvv/base/error.h"

namespace dvv::platform {

// Flushed flushes the new file's data, not the replace itself: macOS and Linux do not flush the
// folder after the rename, and ReplaceFileW is not write-through (the MoveFileExW to a new target
// is), so after a power loss the old file can come back; either file is complete.
enum class FileDurability : std::uint8_t {
    Flushed,    // the bytes reach the disk before the replace: FlushFileBuffers (Windows), fsync
                // (Linux), F_FULLFSYNC, else fsync (macOS)
    Unflushed,  // no flush: after a power loss the reader must detect a torn file itself
};

// What happens to the temporary file when Windows reports that the target was moved away and the
// new file could not take its name (ReplaceFileW 1177, or 1176 when the finishing move fails).
enum class LostTarget : std::uint8_t {
    KeepTemporary,    // the temporary file may be the only copy: it stays, the error names it
    RemoveTemporary,  // the contents can be made again (a cache): it is removed
};

struct ReplaceFileOptions {
    FileDurability durability = FileDurability::Flushed;
    // Windows only: how many more times the replace step is tried when it fails with a sharing
    // violation, a lock violation, access denied, ERROR_UNABLE_TO_REMOVE_REPLACED (1175),
    // ERROR_ALREADY_EXISTS / ERROR_FILE_EXISTS for a new target another writer created first, or
    // ERROR_FILE_NOT_FOUND for a target that another writer's replace has briefly moved away.
    // 0: never retried. macOS and Linux ignore it (rename never fails for these reasons).
    std::uint32_t retries = 0;
    // Windows only: the wait before each retry.
    std::chrono::milliseconds retryDelay{10};
    LostTarget lostTarget = LostTarget::KeepTemporary;
};

// Writes `bytes` to a new temporary file "<target file name>.<16 hex digits>.tmp" in the target's
// folder and replaces `target` with it. The temporary file is created exclusively; on macOS and
// Linux it gets the target's permission bits (a new target: 0666 less the umask). Windows replaces
// an existing target with ReplaceFileW (its attributes and ACL stay) and moves the file to a new
// target with MoveFileExW(MOVEFILE_WRITE_THROUGH); macOS and Linux rename, and keep only the
// target's permission bits (the owner and group are the caller's; ACLs and extended attributes are
// not kept). Creates no folder and resolves no link: a target that exists and is not a regular
// file (a link, a folder) is refused. A read-only target: macOS and Linux replace it (the folder's
// permissions decide); Windows refuses it with access denied, a retried code, so it fails after
// all `retries` attempts. The temporary file is removed on every failure, except a lost target
// with LostTarget::KeepTemporary.
//
// Errc::Io "replacing '<target>' failed: <reason>", the reason one of: "not a regular file",
// "cannot read the file's status: …", "cannot create a temporary file: …", "cannot set the
// permissions: …", "cannot write: …", "cannot flush: …", "cannot create the file: …" (Windows, a
// new target), "cannot replace the file: …", and for a kept temporary file "cannot replace the
// file: …; the new contents are in <temporary path>". Errc::Internal "replaceFile: '<target>'
// names no file" for a path without a file name. Paths in messages are display text
// (dvv::pathDisplayText). Thread-safe: no shared
// state; concurrent calls on one target leave one complete file (the last replace wins). Throws
// only std::bad_alloc.
[[nodiscard]] Result<void> replaceFile(const std::filesystem::path& target,
                                       std::span<const std::byte> bytes,
                                       const ReplaceFileOptions& options = {});

}  // namespace dvv::platform
