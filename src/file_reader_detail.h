#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>

#include "dvv/base/error.h"
#include "dvv/platform/file_reader.h"

namespace dvv::platform::detail {

inline constexpr std::intptr_t NO_HANDLE = -1;

struct NativeOpen {
    std::intptr_t handle = NO_HANDLE;
    int error = 0;  // errno or GetLastError() when handle == NO_HANDLE
};
struct NativeSize {
    std::uint64_t size = 0;
    int error = 0;  // EISDIR / ERROR_* when the handle is not a readable regular file
};
struct NativeRead {
    std::size_t bytes = 0;  // bytes read before the error or the end of the file
    int error = 0;
};

// Platform parts (file_reader_win32.cpp, file_reader_posix.cpp).
// Whether FileReadMode::Auto tries unbuffered reads first (Windows only, U-IO2).
extern const bool AUTO_TRIES_UNBUFFERED;
// Opens buffered with the sequential-access hint, or unbuffered (see file_reader.h).
[[nodiscard]] NativeOpen openNative(const std::filesystem::path& path, bool unbuffered) noexcept;
// The flags openNative passes: CreateFileW's dwFlagsAndAttributes on Windows, open()'s flags on
// Linux (the Linux hint is posix_fadvise, not a flag). For tests.
[[nodiscard]] std::uint32_t nativeOpenFlags(bool unbuffered) noexcept;
void closeNative(std::intptr_t handle) noexcept;
[[nodiscard]] NativeSize fileSize(std::intptr_t handle) noexcept;
// The unbuffered I/O granule of the handle's volume: Windows max(LogicalBytesPerSector,
// PhysicalBytesPerSectorForAtomicity) of FileStorageInfo, Linux statx STATX_DIOALIGN (4096 when
// the kernel does not report it); 0 when it cannot be queried.
[[nodiscard]] std::uint32_t sectorSize(std::intptr_t handle) noexcept;
// Reads until `destination` is full or offset + bytes reaches `file_size`; retries EINTR and
// partial reads.
[[nodiscard]] NativeRead readNative(std::intptr_t handle, std::uint64_t offset,
                                    std::span<std::byte> destination,
                                    std::uint64_t file_size) noexcept;
[[nodiscard]] std::string systemMessage(int error);  // UTF-8, with the number

// UTF-8 for messages (path::u8string, never path::string, which uses the ANSI code page).
[[nodiscard]] std::string displayPath(const std::filesystem::path& path);

// The platform calls of FileReader::open: the test seam that injects open-time failures (the
// U-IO2 fallback) and counts reads in flight. The reader keeps `read` for readAt().
struct NativeOps {
    NativeOpen (*open)(const std::filesystem::path&, bool unbuffered) noexcept = openNative;
    std::uint32_t (*sectorSize)(std::intptr_t handle) noexcept = detail::sectorSize;
    NativeReadFn read = readNative;
    bool autoTriesUnbuffered = AUTO_TRIES_UNBUFFERED;
};
[[nodiscard]] Result<FileReader> openWith(const std::filesystem::path& path, FileReadMode mode,
                                          const NativeOps& ops);

struct FileReaderAccess {
    [[nodiscard]] static FileReader make(std::intptr_t handle, std::filesystem::path path,
                                         std::uint64_t size, std::uint32_t alignment,
                                         std::string fallback, NativeReadFn read,
                                         std::unique_ptr<std::mutex> read_lock) noexcept {
        return FileReader(handle, std::move(path), size, alignment, std::move(fallback), read,
                          std::move(read_lock));
    }
};

}  // namespace dvv::platform::detail
