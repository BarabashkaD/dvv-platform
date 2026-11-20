#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <source_location>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "dvv/base/error.h"
#include "dvv/base/utf8.h"
#include "dynamic_library_detail.h"
#include "file_writer_detail.h"

namespace dvv::platform::detail {

namespace {

// One WriteFile moves at most this much (a DWORD count).
constexpr std::size_t MAX_CHUNK = std::size_t{1} << 30;

std::unexpected<WriteFailure> ioFailure(
    std::string message, bool name_taken = false,
    std::source_location where = std::source_location::current()) {
    return std::unexpected(WriteFailure{
        .error = makeError(Errc::Io, std::move(message), where).error(), .nameTaken = name_taken});
}

// Whether a failed replace step may be tried again: these codes leave both files intact. 183 and
// 80 come from a move to a new target that another writer created first.
bool isRetryable(DWORD code, bool new_target) {
    switch (code) {
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
        case ERROR_ACCESS_DENIED:
        case ERROR_UNABLE_TO_REMOVE_REPLACED: return true;
        case ERROR_ALREADY_EXISTS:
        case ERROR_FILE_EXISTS: return new_target;
        default: return false;
    }
}

// The target was moved away and `temporary` could not take its name: `lost_target` decides
// whether the temporary file, which may be the only copy, stays.
std::unexpected<WriteFailure> lostTargetFailure(const std::filesystem::path& temporary, DWORD code,
                                                LostTarget lost_target) {
    if (lost_target == LostTarget::RemoveTemporary) {
        DeleteFileW(temporary.c_str());
        return ioFailure(std::format("cannot replace the file: {}", win32ErrorMessage(code)));
    }
    return ioFailure(std::format("cannot replace the file: {}; the new contents are in {}",
                                 win32ErrorMessage(code), pathDisplayText(temporary)));
}

// Replaces `target` with the closed file `temporary`, trying again after a retryable code while
// attempts are left. On failure it removes `temporary`, except a lost target with
// LostTarget::KeepTemporary.
std::expected<void, WriteFailure> replaceTarget(const std::filesystem::path& target,
                                                const std::filesystem::path& temporary,
                                                bool target_exists,
                                                const ReplaceFileOptions& options) {
    bool exists = target_exists;
    for (std::uint32_t attempt = 0;; ++attempt) {
        // Read again per attempt: another writer may have created the target meanwhile. When the
        // read fails, the last answer stays.
        std::error_code error;
        const std::filesystem::file_type type =
            std::filesystem::symlink_status(target, error).type();
        if (type != std::filesystem::file_type::none) {
            exists = type != std::filesystem::file_type::not_found;
        }
        // ReplaceFileW keeps the target's attributes and ACL.
        const BOOL replaced =
            exists ? ReplaceFileW(target.c_str(), temporary.c_str(), nullptr,
                                  REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)
                   : MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH);
        if (replaced != FALSE) {
            return {};
        }
        const DWORD code = GetLastError();
        if (isRetryable(code, !exists) && attempt < options.retries) {
            std::this_thread::sleep_for(options.retryDelay);
            continue;
        }
        if (!exists) {
            DeleteFileW(temporary.c_str());
            return ioFailure(std::format("cannot create the file: {}", win32ErrorMessage(code)));
        }
        if (code == ERROR_UNABLE_TO_MOVE_REPLACEMENT) {
            // Without a backup file, 1176 means the target is gone and the temporary file still
            // has its own name: the move finishes the replace (not a retry).
            if (MoveFileExW(temporary.c_str(), target.c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE) {
                return {};
            }
            return lostTargetFailure(temporary, GetLastError(), options.lostTarget);
        }
        if (code == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2) {  // 1177: the target has another name
            return lostTargetFailure(temporary, code, options.lostTarget);
        }
        DeleteFileW(temporary.c_str());
        return ioFailure(std::format("cannot replace the file: {}", win32ErrorMessage(code)));
    }
}

}  // namespace

std::expected<void, WriteFailure> writeAndReplace(const std::filesystem::path& target,
                                                  const std::filesystem::path& temporary,
                                                  bool target_exists,
                                                  std::span<const std::byte> bytes,
                                                  const ReplaceFileOptions& options) {
    // No security attributes: the temporary file inherits the folder's ACL, as any new file in it
    // does, so it is no wider than a new target. ReplaceFileW then gives it the target's ACL; it
    // needs WRITE_DAC on the temporary file, which its owner (this process) has.
    const HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        return ioFailure(std::format("cannot create a temporary file: {}", win32ErrorMessage(code)),
                         code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS);
    }
    std::string failure;
    std::span<const std::byte> rest = bytes;
    while (failure.empty() && !rest.empty()) {
        const auto chunk = static_cast<DWORD>(std::min(rest.size(), MAX_CHUNK));
        DWORD written = 0;
        if (WriteFile(file, rest.data(), chunk, &written, nullptr) == FALSE) {
            failure = std::format("cannot write: {}", win32ErrorMessage(GetLastError()));
        } else if (written == 0) {
            failure = "cannot write: no bytes were written";
        } else {
            rest = rest.subspan(written);
        }
    }
    if (failure.empty() && options.durability == FileDurability::Flushed &&
        FlushFileBuffers(file) == FALSE) {
        failure = std::format("cannot flush: {}", win32ErrorMessage(GetLastError()));
    }
    if (CloseHandle(file) == FALSE && failure.empty()) {
        failure = std::format("cannot write: {}", win32ErrorMessage(GetLastError()));
    }
    if (!failure.empty()) {
        DeleteFileW(temporary.c_str());
        return ioFailure(std::move(failure));
    }
    return replaceTarget(target, temporary, target_exists, options);
}

}  // namespace dvv::platform::detail
