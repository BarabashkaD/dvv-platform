#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "dvv/platform/aligned_buffer.h"

TEST_CASE("allocateAligned returns aligned memory") {
    const std::array<std::pair<std::size_t, std::size_t>, 4> cases{
        {{1, 1}, {100, 512}, {8192, 4096}, {5000, 65536}}};
    for (const auto& [bytes, alignment] : cases) {
        CAPTURE(bytes);
        CAPTURE(alignment);
        const auto buffer = dvv::platform::allocateAligned(bytes, alignment);
        CHECK(buffer.has_value());
        if (buffer) {
            CHECK(reinterpret_cast<std::uintptr_t>(buffer->get()) % alignment == 0);
        }
    }
    const auto empty = dvv::platform::allocateAligned(0, 64);
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error().code == dvv::Errc::Internal);
    const auto odd = dvv::platform::allocateAligned(64, 3);
    REQUIRE_FALSE(odd.has_value());
    CHECK(odd.error().code == dvv::Errc::Internal);
}
