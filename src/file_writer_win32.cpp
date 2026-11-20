#include "file_writer_win32.h"

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

// Owns the new temporary file: closes it and removes it unless keep() was called, also when an
// exception (std::bad_alloc) leaves writeAndReplace.
class TemporaryFile {
public:
    TemporaryFile(HANDLE file, const std::filesystem::path& path) noexcept
        : file_(file), path_(path) {}
    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;
    TemporaryFile(TemporaryFile&&) = delete;
    TemporaryFile& operator=(TemporaryFile&&) = delete;
    ~TemporaryFile() {
        if (file_ != INVALID_HANDLE_VALUE) {
            CloseHandle(file_);
        }
        if (remove_) {
            DeleteFileW(path_.c_str());
        }
    }

    [[nodiscard]] HANDLE get() const noexcept { return file_; }
    // Closes the file; false with GetLastError() set when CloseHandle fails.
    [[nodiscard]] bool close() noexcept {
        return CloseHandle(std::exchange(file_, INVALID_HANDLE_VALUE)) != FALSE;
    }
    // The file now has the target's name, or it is the kept copy of a lost target.
    void keep() noexcept { remove_ = false; }

private:
    HANDLE file_;
    const std::filesystem::path& path_;
    bool remove_ = true;
};

// The target was moved away and the temporary file could not take its name: `lost_target` decides
// whether the temporary file, which may be the only copy, stays.
std::unexpected<WriteFailure> lostTargetFailure(TemporaryFile& file,
                                                const std::filesystem::path& temporary, DWORD code,
                                                LostTarget lost_target) {
    if (lost_target == LostTarget::RemoveTemporary) {
        return ioFailure(std::format("cannot replace the file: {}", win32ErrorMessage(code)));
    }
    auto failure = ioFailure(std::format("cannot replace the file: {}; the new contents are in {}",
                                         win32ErrorMessage(code), pathDisplayText(temporary)));
    file.keep();
    return failure;
}

// Replaces `target` with the closed temporary file, trying again after a retryable code while
// attempts are left. `file` removes the temporary file on failure, except a lost target with
// LostTarget::KeepTemporary.
std::expected<void, WriteFailure> replaceTarget(const std::filesystem::path& target,
                                                const std::filesystem::path& temporary,
                                                bool target_exists,
                                                const ReplaceFileOptions& options,
                                                TemporaryFile& file) {
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
            file.keep();
            return {};
        }
        const DWORD code = GetLastError();
        if (isRetryableReplaceError(code, !exists) && attempt < options.retries) {
            std::this_thread::sleep_for(options.retryDelay);
            continue;
        }
        if (!exists) {
            return ioFailure(std::format("cannot create the file: {}", win32ErrorMessage(code)));
        }
        if (code == ERROR_UNABLE_TO_MOVE_REPLACEMENT) {
            // Without a backup file, 1176 means the target is gone and the temporary file still
            // has its own name: the move finishes the replace (not a retry).
            if (MoveFileExW(temporary.c_str(), target.c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE) {
                file.keep();
                return {};
            }
            return lostTargetFailure(file, temporary, GetLastError(), options.lostTarget);
        }
        if (code == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2) {  // 1177: the target has another name
            return lostTargetFailure(file, temporary, code, options.lostTarget);
        }
        return ioFailure(std::format("cannot replace the file: {}", win32ErrorMessage(code)));
    }
}

}  // namespace

bool isRetryableReplaceError(unsigned long code, bool new_target) noexcept {
    switch (code) {
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
        case ERROR_ACCESS_DENIED:
        case ERROR_UNABLE_TO_REMOVE_REPLACED: return true;
        case ERROR_ALREADY_EXISTS:
        case ERROR_FILE_EXISTS: return new_target;
        case ERROR_FILE_NOT_FOUND: return !new_target;
        default: return false;
    }
}

std::expected<void, WriteFailure> writeAndReplace(const std::filesystem::path& target,
                                                  const std::filesystem::path& temporary,
                                                  bool target_exists,
                                                  std::span<const std::byte> bytes,
                                                  const ReplaceFileOptions& options) {
    // No security attributes: the temporary file inherits the folder's ACL, as any new file in it
    // does, so it is no wider than a new target. ReplaceFileW then gives it the target's ACL; it
    // needs WRITE_DAC on the temporary file, which its owner (this process) has.
    const HANDLE created = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                       FILE_ATTRIBUTE_NORMAL, nullptr);
    if (created == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        return ioFailure(std::format("cannot create a temporary file: {}", win32ErrorMessage(code)),
                         code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS);
    }
    TemporaryFile file(created, temporary);
    std::span<const std::byte> rest = bytes;
    while (!rest.empty()) {
        const auto chunk = static_cast<DWORD>(std::min(rest.size(), MAX_CHUNK));
        DWORD written = 0;
        if (WriteFile(file.get(), rest.data(), chunk, &written, nullptr) == FALSE) {
            return ioFailure(std::format("cannot write: {}", win32ErrorMessage(GetLastError())));
        }
        if (written == 0) {
            return ioFailure("cannot write: no bytes were written");
        }
        rest = rest.subspan(written);
    }
    if (options.durability == FileDurability::Flushed && FlushFileBuffers(file.get()) == FALSE) {
        return ioFailure(std::format("cannot flush: {}", win32ErrorMessage(GetLastError())));
    }
    if (!file.close()) {
        return ioFailure(std::format("cannot write: {}", win32ErrorMessage(GetLastError())));
    }
    return replaceTarget(target, temporary, target_exists, options, file);
}

}  // namespace dvv::platform::detail
