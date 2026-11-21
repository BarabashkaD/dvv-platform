#include <doctest/doctest.h>

#include "dvv/platform/pixel_size.h"

static_assert(dvv::platform::PixelSize{}.empty());
static_assert(dvv::platform::PixelSize{.width = 2, .height = 0}.empty());
static_assert(dvv::platform::PixelSize{.width = 2, .height = 3} ==
              dvv::platform::PixelSize{.width = 2, .height = 3});

TEST_CASE("a pixel size is empty when a side is 0") {
    using dvv::platform::PixelSize;
    CHECK(PixelSize{}.empty());
    CHECK(PixelSize{.width = 1, .height = 0}.empty());
    CHECK(PixelSize{.width = 0, .height = 1}.empty());
    CHECK_FALSE(PixelSize{.width = 1, .height = 1}.empty());
    CHECK(PixelSize{.width = 2, .height = 3} == PixelSize{.width = 2, .height = 3});
    CHECK(PixelSize{.width = 2, .height = 3} != PixelSize{.width = 3, .height = 2});
}
