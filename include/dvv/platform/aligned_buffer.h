#pragma once

#include <cstddef>
#include <memory>

#include "dvv/base/error.h"

namespace dvv::platform {
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
