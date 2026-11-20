#include "file_writer_posix.h"

#include <algorithm>
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

}  // namespace

mode_t temporaryMode(const std::filesystem::path& target, bool target_exists) {
    if (!target_exists) {
        return 0666;
    }
    struct stat status{};
    return static_cast<mode_t>(stat(target.c_str(), &status) == 0 ? (status.st_mode & 0777) : 0600);
}

std::expected<void, WriteFailure> writeAndReplace(const std::filesystem::path& target,
                                                  const std::filesystem::path& temporary,
                                                  bool target_exists,
                                                  std::span<const std::byte> bytes,
                                                  const ReplaceFileOptions& options) {
    // Created with the target's mode, never wider: the new contents are never readable by more
    // users than the old ones.
    const mode_t mode = temporaryMode(target, target_exists);
    const int file = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
    if (file < 0) {
        const int error = errno;
        return ioFailure(std::format("cannot create a temporary file: {}", systemMessage(error)),
                         error == EEXIST);
    }
    std::string failure;
    // open() applied the umask: an existing target's bits that it removed come back. This only
    // adds bits, up to the target's mode.
    if (target_exists && fchmod(file, mode) != 0) {
        failure = std::format("cannot set the permissions: {}", systemMessage(errno));
    }
    std::span<const std::byte> rest = bytes;
    while (failure.empty() && !rest.empty()) {
        const ssize_t written = write(file, rest.data(), std::min(rest.size(), MAX_CHUNK));
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written < 0) {
            failure = std::format("cannot write: {}", systemMessage(errno));
        } else if (written == 0) {
            failure = "cannot write: no bytes were written";
        } else {
            rest = rest.subspan(static_cast<std::size_t>(written));
        }
    }
    if (failure.empty() && options.durability == FileDurability::Flushed && !flushToDisk(file)) {
        failure = std::format("cannot flush: {}", systemMessage(errno));
    }
    if (close(file) != 0 && failure.empty()) {
        failure = std::format("cannot write: {}", systemMessage(errno));
    }
    if (failure.empty() && std::rename(temporary.c_str(), target.c_str()) != 0) {
        failure = std::format("cannot replace the file: {}", systemMessage(errno));
    }
    if (!failure.empty()) {
        unlink(temporary.c_str());
        return ioFailure(std::move(failure));
    }
    return {};
}

}  // namespace dvv::platform::detail
