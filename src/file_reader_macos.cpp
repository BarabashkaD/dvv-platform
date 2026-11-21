#include <cerrno>
#include <cstdint>

#include <fcntl.h>

#include "file_reader_detail.h"
#include "file_reader_posix.h"

namespace dvv::platform::detail {

// Auto reads buffered here (U-IO2); only an explicit Unbuffered uses F_NOCACHE.
const bool AUTO_TRIES_UNBUFFERED = false;

std::uint32_t nativeOpenFlags(OpenMode /*mode*/) noexcept {
    // No O_DIRECT on macOS: nativeFcntlHints() says what applyOpenHints sets after open.
    return static_cast<std::uint32_t>(O_RDONLY | O_CLOEXEC);
}

// F_NOCACHE for unbuffered, F_RDAHEAD for the buffered sequential hint. Neither can be read back,
// so tests see them here.
FcntlHints nativeFcntlHints(OpenMode mode) noexcept {
    const bool unbuffered = mode == OpenMode::Unbuffered;
    return {.noCache = unbuffered, .readAhead = !unbuffered};
}

int applyOpenHints(int fd, OpenMode mode) noexcept {
    const FcntlHints hints = nativeFcntlHints(mode);
    // NOLINTBEGIN(cppcoreguidelines-pro-type-vararg)
    if (hints.noCache && ::fcntl(fd, F_NOCACHE, 1) != 0) {
        // Not a hint here: without it the read is silently buffered, so the open fails.
        return errno;
    }
    if (hints.readAhead) {
        // A hint only; a failure changes nothing that is checked.
        static_cast<void>(::fcntl(fd, F_RDAHEAD, 1));
    }
    // NOLINTEND(cppcoreguidelines-pro-type-vararg)
    return 0;
}

std::uint32_t sectorSize(std::intptr_t /*handle*/) noexcept {
    return 4096;  // F_NOCACHE needs no alignment; the contract of alignment() stays the same
}

}  // namespace dvv::platform::detail
