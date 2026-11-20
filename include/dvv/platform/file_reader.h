#pragma once

// Positional file reads for frame data (M2 design D8, spec §10.4; M3.1 U-IO2). A FileReader is a
// move-only handle with a blocking readAt() that any thread may call; one read runs at a time per
// reader, so a file source never has more than one read in flight (U-IO2). Callers run reads on
// an I/O thread pool (co_await resumeOn(io)).
//
// Two modes behind one interface (U-IO2):
// - Buffered (portable): reads go through the OS file cache with a sequential-access hint.
//   Windows CreateFileW(GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_EXISTING,
//   FILE_FLAG_SEQUENTIAL_SCAN); Linux open(O_RDONLY | O_CLOEXEC) and
//   posix_fadvise(POSIX_FADV_SEQUENTIAL); macOS open(O_RDONLY | O_CLOEXEC) and fcntl(F_RDAHEAD).
// - Unbuffered: no OS cache; offset, size and address are multiples of alignment(). Windows
//   FILE_FLAG_NO_BUFFERING; Linux O_DIRECT; macOS fcntl(F_NOCACHE) (which needs no alignment, but
//   the same alignment() of 4096 is kept so callers behave the same on every platform).
// Reads are synchronous and positional: on Windows a ReadFile on a synchronous handle with an
// OVERLAPPED that carries only the offset; Linux and macOS use pread().

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>

#include "dvv/base/error.h"
#include "dvv/platform/aligned_buffer.h"

namespace dvv::platform {

namespace detail {
struct FileReaderAccess;
struct NativeRead;
using NativeReadFn = NativeRead (*)(std::intptr_t handle, std::uint64_t offset,
                                    std::span<std::byte> destination,
                                    std::uint64_t file_size) noexcept;

// What a FileReader holds (complete here because FileReader holds it).
struct ReaderParts {
    std::intptr_t handle = -1;  // HANDLE (Windows) or file descriptor; -1: none
    std::filesystem::path path;
    std::uint64_t size = 0;
    std::uint32_t alignment = 1;  // > 1: unbuffered
    std::string fallback;
    NativeReadFn read = nullptr;           // the platform read (a test seam)
    std::unique_ptr<std::mutex> readLock;  // one read at a time (U-IO2); null when moved from
};
}  // namespace detail

enum class FileReadMode : std::uint8_t {
    // Windows: Unbuffered when the open-time check passes (the volume's sector size, the
    // unbuffered open, the first aligned read), else Buffered for this file with one log line
    // saying why. Other platforms: Buffered.
    Auto,
    Buffered,    // through the OS file cache (also small header reads)
    Unbuffered,  // the open-time check must pass: a failure is an error, never a fallback
};

class FileReader {
public:
    // Errc::Io "'<path>' could not be opened: <system message>" (missing, denied, a directory).
    // An explicit Unbuffered whose check fails: Errc::Io "'<path>' cannot be read unbuffered:
    // <reason>". The size is read once at open.
    [[nodiscard]] static Result<FileReader> open(const std::filesystem::path& path,
                                                 FileReadMode mode = FileReadMode::Buffered);
    FileReader(FileReader&& other) noexcept;
    FileReader& operator=(FileReader&& other) noexcept;
    FileReader(const FileReader&) = delete;
    FileReader& operator=(const FileReader&) = delete;
    ~FileReader();

    [[nodiscard]] std::uint64_t size() const noexcept { return parts_.size; }
    [[nodiscard]] bool unbuffered() const noexcept { return parts_.alignment > 1; }
    // Required alignment of offset, length and destination address: 1 when buffered; otherwise
    // max(4096, the volume's sector size).
    [[nodiscard]] std::uint32_t alignment() const noexcept { return parts_.alignment; }
    // Why Auto reads this file buffered; empty otherwise.
    [[nodiscard]] const std::string& fallbackReason() const noexcept { return parts_.fallback; }
    // "unbuffered", "buffered" or "buffered (fallback: <reason>)" (CLI info, the GUI Info pane).
    [[nodiscard]] std::string modeText() const;
    // Blocking read of destination.size() bytes at `offset`; returns the bytes read, fewer only at
    // the end of the file (0 at or past it). Concurrent calls wait for each other (one read at a
    // time). Errc::Internal for a misaligned offset, size or address (unbuffered only) and for a
    // moved-from reader; Errc::Io for a system error (the message names the file and the
    // offset). Retries EINTR and partial reads; never reopens.
    [[nodiscard]] Result<std::size_t> readAt(std::uint64_t offset,
                                             std::span<std::byte> destination) const;
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return parts_.path; }

private:
    friend struct detail::FileReaderAccess;
    explicit FileReader(detail::ReaderParts parts) noexcept;
    void close() noexcept;

    detail::ReaderParts parts_;  // a default ReaderParts is the moved-from state
};

}  // namespace dvv::platform
