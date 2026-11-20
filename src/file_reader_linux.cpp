#include <algorithm>
#include <cstdint>

#include <fcntl.h>
#include <sys/stat.h>

#include "file_reader_detail.h"
#include "file_reader_posix.h"

namespace dvv::platform::detail {

// Auto reads buffered here (U-IO2); only an explicit Unbuffered uses O_DIRECT.
const bool AUTO_TRIES_UNBUFFERED = false;

std::uint32_t nativeOpenFlags(OpenMode mode) noexcept {
    return static_cast<std::uint32_t>(O_RDONLY | O_CLOEXEC |
                                      (mode == OpenMode::Unbuffered ? O_DIRECT : 0));
}

// Linux uses O_DIRECT in the open flags and posix_fadvise, no fcntl hint.
FcntlHints nativeFcntlHints(OpenMode /*mode*/) noexcept {
    return {};
}

int applyOpenHints(int fd, OpenMode mode) noexcept {
    if (mode == OpenMode::Buffered) {
        // A hint only (the kernel reads ahead more); a failure changes nothing that is checked.
        static_cast<void>(::posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL));
    }
    return 0;
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

}  // namespace dvv::platform::detail
