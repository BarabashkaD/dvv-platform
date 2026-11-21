// src/file_reader_posix.h (private; macOS and Linux)
#pragma once

#include "file_reader_detail.h"  // OpenMode

namespace dvv::platform::detail {
struct FcntlHints {
    bool noCache = false;
    bool readAhead = false;
};
[[nodiscard]] FcntlHints nativeFcntlHints(OpenMode mode) noexcept;
// The hints after open(): 0, or the errno of a hint that must succeed (F_NOCACHE on macOS).
[[nodiscard]] int applyOpenHints(int fd, OpenMode mode) noexcept;
}  // namespace dvv::platform::detail
