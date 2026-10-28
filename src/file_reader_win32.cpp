#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "dynamic_library_detail.h"
#include "file_reader_detail.h"

namespace dvv::platform::detail {

namespace {

HANDLE toHandle(std::intptr_t handle) noexcept {
    return reinterpret_cast<HANDLE>(handle);  // NOLINT(performance-no-int-to-ptr)
}

// One ReadFile moves at most this much (a DWORD count); a multiple of every alignment.
constexpr std::size_t MAX_CHUNK = std::size_t{1} << 30;

}  // namespace

const bool AUTO_TRIES_UNBUFFERED = true;

std::uint32_t nativeOpenFlags(bool unbuffered) noexcept {
    // A synchronous handle: FileReader runs one read at a time, so overlapped I/O gains nothing.
    // Buffered: the sequential-scan hint makes the cache manager read ahead more. Unbuffered: no
    // cache, sector-aligned I/O.
    return FILE_ATTRIBUTE_NORMAL |
           (unbuffered ? FILE_FLAG_NO_BUFFERING : FILE_FLAG_SEQUENTIAL_SCAN);
}

NativeOpen openNative(const std::filesystem::path& path, bool unbuffered) noexcept {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, nativeOpenFlags(unbuffered), nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return {.handle = NO_HANDLE, .error = static_cast<int>(GetLastError())};
    }
    return {.handle = reinterpret_cast<std::intptr_t>(file), .error = 0};
}

void closeNative(std::intptr_t handle) noexcept {
    if (handle != NO_HANDLE) {
        CloseHandle(toHandle(handle));
    }
}

NativeSize fileSize(std::intptr_t handle) noexcept {
    LARGE_INTEGER size{};
    if (GetFileSizeEx(toHandle(handle), &size) == 0) {
        return {.size = 0, .error = static_cast<int>(GetLastError())};
    }
    return {.size = static_cast<std::uint64_t>(size.QuadPart), .error = 0};
}

std::uint32_t sectorSize(std::intptr_t handle) noexcept {
    FILE_STORAGE_INFO info{};
    if (GetFileInformationByHandleEx(toHandle(handle), FileStorageInfo, &info, sizeof(info)) == 0) {
        return 0;
    }
    return std::max<std::uint32_t>(info.LogicalBytesPerSector,
                                   info.PhysicalBytesPerSectorForAtomicity);
}

NativeRead readNative(std::intptr_t handle, std::uint64_t offset, std::span<std::byte> destination,
                      std::uint64_t file_size) noexcept {
    const HANDLE file = toHandle(handle);
    std::size_t done = 0;
    int error = 0;
    while (done < destination.size() && offset + done < file_size) {
        const auto chunk =
            static_cast<DWORD>(std::min<std::size_t>(destination.size() - done, MAX_CHUNK));
        const std::uint64_t at = offset + done;
        // A synchronous handle: the OVERLAPPED only carries the offset (a blocking read).
        OVERLAPPED position{};
        position.Offset = static_cast<DWORD>(at & 0xFFFFFFFFU);
        position.OffsetHigh = static_cast<DWORD>(at >> 32);
        DWORD transferred = 0;
        if (ReadFile(file, destination.data() + done, chunk, &transferred, &position) == 0) {
            const DWORD failed = GetLastError();
            if (failed != ERROR_HANDLE_EOF) {
                error = static_cast<int>(failed);
            }
            break;
        }
        if (transferred == 0) {
            break;  // end of file
        }
        done += transferred;
    }
    return {.bytes = done, .error = error};
}

std::string systemMessage(int error) {
    // The UTF-8 FormatMessageW helper that DynamicLibrary already uses: "<text> (error <n>)".
    return win32ErrorMessage(static_cast<DWORD>(error));
}

}  // namespace dvv::platform::detail
