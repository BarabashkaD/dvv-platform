#pragma once

#include <cstdint>

namespace dvv::platform {
struct PixelSize {  // physical pixels
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    [[nodiscard]] constexpr bool empty() const noexcept { return width == 0 || height == 0; }
    friend constexpr bool operator==(PixelSize, PixelSize) noexcept = default;
};
}  // namespace dvv::platform
