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
//   posix_fadvise(POSIX_FADV_SEQUENTIAL).
// - Unbuffered: no OS cache; offset, size and address are multiples of alignment(). Windows
//   FILE_FLAG_NO_BUFFERING; Linux O_DIRECT.
// Reads are synchronous and positional: on Windows a ReadFile on a synchronous handle with an
// OVERLAPPED that carries only the offset; Linux uses pread().

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>

#include "dvv/base/error.h"

namespace dvv::platform {

namespace detail {
struct FileReaderAccess;
struct NativeRead;
using NativeReadFn = NativeRead (*)(std::intptr_t handle, std::uint64_t offset,
                                    std::span<std::byte> destination,
                                    std::uint64_t file_size) noexcept;
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

    [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
    [[nodiscard]] bool unbuffered() const noexcept { return unbuffered_; }
    // Required alignment of offset, length and destination address: 1 when buffered; otherwise
    // max(4096, the volume's sector size).
    [[nodiscard]] std::uint32_t alignment() const noexcept { return alignment_; }
    // Why Auto reads this file buffered; empty otherwise.
    [[nodiscard]] const std::string& fallbackReason() const noexcept { return fallback_; }
    // "unbuffered", "buffered" or "buffered (fallback: <reason>)" (CLI info, the GUI Info pane).
    [[nodiscard]] std::string modeText() const;
    // Blocking read of destination.size() bytes at `offset`; returns the bytes read, fewer only at
    // the end of the file (0 at or past it). Concurrent calls wait for each other (one read at a
    // time). Errc::Internal for a misaligned offset, size or address (unbuffered only) and for a
    // moved-from reader; Errc::Io for a system error (the message names the file and the
    // offset). Retries EINTR and partial reads; never reopens.
    [[nodiscard]] Result<std::size_t> readAt(std::uint64_t offset,
                                             std::span<std::byte> destination) const;
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    friend struct detail::FileReaderAccess;
    FileReader(std::intptr_t handle, std::filesystem::path path, std::uint64_t size,
               std::uint32_t alignment, std::string fallback, detail::NativeReadFn read,
               std::unique_ptr<std::mutex> read_lock) noexcept;
    void close() noexcept;

    std::intptr_t handle_ = -1;  // HANDLE (Windows) or file descriptor (Linux); -1: none
    std::filesystem::path path_;
    std::uint64_t size_ = 0;
    std::uint32_t alignment_ = 1;
    bool unbuffered_ = false;
    std::string fallback_;
    detail::NativeReadFn read_ = nullptr;    // the platform read (a test seam)
    std::unique_ptr<std::mutex> read_lock_;  // one read at a time (U-IO2); null when moved from
};

// Aligned host memory (operator new[] with std::align_val_t): unbuffered read targets and host
// frames.
struct AlignedBufferDeleter {
    std::size_t alignment = 1;
    void operator()(std::byte* memory) const noexcept;
};
using AlignedBuffer = std::unique_ptr<std::byte[], AlignedBufferDeleter>;
// Errc::Internal for 0 bytes or an alignment that is not a power of two; Errc::OutOfMemory when
// the allocation fails.
[[nodiscard]] Result<AlignedBuffer> allocateAligned(std::size_t bytes, std::size_t alignment);

}  // namespace dvv::platform
