#include "dvv/platform/aligned_buffer.h"

#include <bit>
#include <cstddef>
#include <format>
#include <new>

#include "dvv/base/error.h"

namespace dvv::platform {

Result<AlignedBuffer> allocateAligned(std::size_t bytes, std::size_t alignment) {
    if (bytes == 0 || !std::has_single_bit(alignment)) {
        return makeError(Errc::Internal,
                         std::format("allocateAligned: {} bytes at alignment {}: the size must not "
                                     "be 0 and the alignment must be a power of two",
                                     bytes, alignment));
    }
    void* memory = ::operator new[](bytes, std::align_val_t{alignment}, std::nothrow);
    if (memory == nullptr) {
        return makeError(Errc::OutOfMemory,
                         std::format("allocating {} bytes aligned to {} failed", bytes, alignment));
    }
    return AlignedBuffer(static_cast<std::byte*>(memory), AlignedBufferDeleter{alignment});
}

void AlignedBufferDeleter::operator()(std::byte* memory) const noexcept {
    ::operator delete[](memory, std::align_val_t{alignment});
}

}  // namespace dvv::platform
