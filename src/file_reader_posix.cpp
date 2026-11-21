#include "file_reader_posix.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <span>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "file_reader_detail.h"

// The POSIX part shared by macOS and Linux; the open flags, the open hints and the sector size are
// in file_reader_macos.cpp and file_reader_linux.cpp.

namespace dvv::platform::detail {

namespace {
constexpr std::size_t MAX_CHUNK = std::size_t{1} << 30;  // below Linux's 0x7ffff000 per call
}  // namespace

NativeOpen openNative(const std::filesystem::path& path, OpenMode mode) noexcept {
    int fd = -1;
    do {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        fd = ::open(path.c_str(), static_cast<int>(nativeOpenFlags(mode)));
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        return {.handle = NO_HANDLE, .error = errno};
    }
    const int error = applyOpenHints(fd, mode);
    if (error != 0) {
        ::close(fd);
        return {.handle = NO_HANDLE, .error = error};
    }
    return {.handle = fd, .error = 0};
}

void closeNative(std::intptr_t handle) noexcept {
    if (handle != NO_HANDLE) {
        ::close(static_cast<int>(handle));
    }
}

NativeSize fileSize(std::intptr_t handle) noexcept {
    struct stat info{};
    if (::fstat(static_cast<int>(handle), &info) != 0) {
        return {.size = 0, .error = errno};
    }
    if (S_ISDIR(info.st_mode)) {
        return {.size = 0, .error = EISDIR};
    }
    return {.size = static_cast<std::uint64_t>(info.st_size), .error = 0};
}

NativeRead readNative(std::intptr_t handle, std::uint64_t offset, std::span<std::byte> destination,
                      std::uint64_t file_size) noexcept {
    std::size_t done = 0;
    while (done < destination.size() && offset + done < file_size) {
        const std::size_t chunk = std::min(destination.size() - done, MAX_CHUNK);
        const ssize_t read = ::pread(static_cast<int>(handle), destination.data() + done, chunk,
                                     static_cast<off_t>(offset + done));
        if (read < 0) {
            if (errno == EINTR) {
                continue;
            }
            return {.bytes = done, .error = errno};
        }
        if (read == 0) {
            break;  // end of file
        }
        done += static_cast<std::size_t>(read);
    }
    return {.bytes = done, .error = 0};
}

std::string systemMessage(int error) {
    return std::format("{} (errno {})", std::generic_category().message(error), error);
}

}  // namespace dvv::platform::detail
