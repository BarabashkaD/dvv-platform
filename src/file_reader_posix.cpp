#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "file_reader_detail.h"

namespace dvv::platform::detail {

namespace {
constexpr std::size_t MAX_CHUNK = std::size_t{1} << 30;  // below Linux's 0x7ffff000 per call
}  // namespace

// Auto reads buffered here (U-IO2); only an explicit Unbuffered uses O_DIRECT.
const bool AUTO_TRIES_UNBUFFERED = false;

std::uint32_t nativeOpenFlags(bool unbuffered) noexcept {
    return static_cast<std::uint32_t>(O_RDONLY | O_CLOEXEC | (unbuffered ? O_DIRECT : 0));
}

NativeOpen openNative(const std::filesystem::path& path, bool unbuffered) noexcept {
    int fd = -1;
    do {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        fd = ::open(path.c_str(), static_cast<int>(nativeOpenFlags(unbuffered)));
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        return {.handle = NO_HANDLE, .error = errno};
    }
    if (!unbuffered) {
        // A hint only (the kernel reads ahead more); a failure changes nothing that is checked.
        static_cast<void>(::posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL));
    }
    return {.handle = fd, .error = 0};
}

std::uint32_t sectorSize(std::intptr_t handle) noexcept {
    std::uint32_t alignment = 4096;  // what O_DIRECT accepts on the common file systems
#if defined(STATX_DIOALIGN)
    struct statx info{};
    if (::statx(static_cast<int>(handle), "", AT_EMPTY_PATH, STATX_DIOALIGN, &info) == 0 &&
        (info.stx_mask & STATX_DIOALIGN) != 0) {
        if (info.stx_dio_offset_align == 0) {
            return 0;  // the file system does not support O_DIRECT for this file
        }
        alignment = std::max(static_cast<std::uint32_t>(info.stx_dio_offset_align),
                             static_cast<std::uint32_t>(info.stx_dio_mem_align));
    }
#else
    static_cast<void>(handle);
#endif
    return alignment;
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
