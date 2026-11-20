#include "file_writer_posix.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <format>
#include <source_location>
#include <span>
#include <string>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dvv/base/error.h"
#include "file_reader_detail.h"
#include "file_writer_detail.h"

namespace dvv::platform::detail {

namespace {

// One write() moves at most this much: macOS refuses a count above INT_MAX.
constexpr std::size_t MAX_CHUNK = std::size_t{1} << 30;

// fsync, through F_FULLFSYNC on macOS (fsync alone does not reach the disk there); a file system
// without F_FULLFSYNC gets plain fsync.
bool flushToDisk(int file) {
#if defined(__APPLE__)
    if (fcntl(file, F_FULLFSYNC) == 0) {
        return true;
    }
#endif
    return fsync(file) == 0;
}

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
    TemporaryFile(int file, const std::filesystem::path& path) noexcept
        : file_(file), path_(path) {}
    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;
    TemporaryFile(TemporaryFile&&) = delete;
    TemporaryFile& operator=(TemporaryFile&&) = delete;
    ~TemporaryFile() {
        if (file_ >= 0) {
            ::close(file_);
        }
        if (remove_) {
            ::unlink(path_.c_str());
        }
    }

    [[nodiscard]] int get() const noexcept { return file_; }
    // Closes the file; false with errno set when close() fails.
    [[nodiscard]] bool close() noexcept { return ::close(std::exchange(file_, -1)) == 0; }
    // The file now has the target's name.
    void keep() noexcept { remove_ = false; }

private:
    int file_;
    const std::filesystem::path& path_;
    bool remove_ = true;
};

// The errors of a mode change on a file system without POSIX modes (ENOTSUP and EOPNOTSUPP are
// one value on Linux).
constexpr std::array NO_MODES = {EPERM, ENOTSUP, EOPNOTSUPP, EINVAL};

// Gives the open file `mode` when open() created it with other bits (the umask removed some): 0, or
// the errno of a failure that counts.
int restoreMode(int file, mode_t mode, ModeSetter set_mode) {
    if (struct stat status{}; fstat(file, &status) == 0 && (status.st_mode & 0777) == mode) {
        return 0;
    }
    if (set_mode(file, mode) == 0) {
        return 0;
    }
    const int error = errno;
    return std::ranges::find(NO_MODES, error) != NO_MODES.end() ? 0 : error;
}

}  // namespace

mode_t temporaryMode(const std::filesystem::path& target, bool target_exists) {
    if (!target_exists) {
        return 0666;
    }
    struct stat status{};
    return static_cast<mode_t>(stat(target.c_str(), &status) == 0 ? (status.st_mode & 0777) : 0600);
}

std::expected<void, WriteFailure> writeAndReplaceWith(
    const std::filesystem::path& target, const std::filesystem::path& temporary, bool target_exists,
    std::span<const std::byte> bytes, const ReplaceFileOptions& options, ModeSetter set_mode) {
    // Created with the target's mode, never wider: the new contents are never readable by more
    // users than the old ones.
    const mode_t mode = temporaryMode(target, target_exists);
    const int created = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
    if (created < 0) {
        const int error = errno;
        return ioFailure(std::format("cannot create a temporary file: {}", systemMessage(error)),
                         error == EEXIST);
    }
    TemporaryFile file(created, temporary);
    // open() applied the umask: an existing target's bits that it removed come back. This only
    // adds bits, up to the target's mode.
    if (const int error = target_exists ? restoreMode(file.get(), mode, set_mode) : 0; error != 0) {
        return ioFailure(std::format("cannot set the permissions: {}", systemMessage(error)));
    }
    std::span<const std::byte> rest = bytes;
    while (!rest.empty()) {
        const ssize_t written = write(file.get(), rest.data(), std::min(rest.size(), MAX_CHUNK));
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written < 0) {
            return ioFailure(std::format("cannot write: {}", systemMessage(errno)));
        }
        if (written == 0) {
            return ioFailure("cannot write: no bytes were written");
        }
        rest = rest.subspan(static_cast<std::size_t>(written));
    }
    if (options.durability == FileDurability::Flushed && !flushToDisk(file.get())) {
        return ioFailure(std::format("cannot flush: {}", systemMessage(errno)));
    }
    if (!file.close()) {
        return ioFailure(std::format("cannot write: {}", systemMessage(errno)));
    }
    if (std::rename(temporary.c_str(), target.c_str()) != 0) {
        return ioFailure(std::format("cannot replace the file: {}", systemMessage(errno)));
    }
    file.keep();
    return {};
}

std::expected<void, WriteFailure> writeAndReplace(const std::filesystem::path& target,
                                                  const std::filesystem::path& temporary,
                                                  bool target_exists,
                                                  std::span<const std::byte> bytes,
                                                  const ReplaceFileOptions& options) {
    return writeAndReplaceWith(target, temporary, target_exists, bytes, options, ::fchmod);
}

}  // namespace dvv::platform::detail
