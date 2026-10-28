#include <doctest/doctest.h>

#include <utility>
#include <variant>

#include "dvv/platform/native_window.h"
#include "dvv/platform/testing/test_window.h"

using dvv::platform::PixelSize;
using dvv::platform::testing::TestWindow;

TEST_CASE("a test window reports, changes and restores its client size") {
    auto created = TestWindow::create({.width = 320, .height = 240});
    REQUIRE(created.has_value());
    TestWindow window = std::move(*created);
    CHECK(window.clientSize() == PixelSize{.width = 320, .height = 240});
    CHECK(dvv::platform::windowSystemOf(window.native()) == dvv::platform::defaultWindowSystem());

    window.resize({.width = 200, .height = 150});
    CHECK(window.clientSize() == PixelSize{.width = 200, .height = 150});

    // Narrower than a default window can be (the smallest size dvv-gpu tests use).
    window.resize({.width = 97, .height = 61});
    CHECK(window.clientSize() == PixelSize{.width = 97, .height = 61});
    window.resize({.width = 200, .height = 150});
    CHECK(window.clientSize() == PixelSize{.width = 200, .height = 150});

    window.minimise();
    window.pump();
#if defined(_WIN32)
    CHECK(window.clientSize().empty());
#endif
    window.restore();
    window.pump();
    CHECK(window.clientSize() == PixelSize{.width = 200, .height = 150});

    TestWindow moved = std::move(window);
    CHECK(moved.clientSize() == PixelSize{.width = 200, .height = 150});
    // A moved-from TestWindow is documented to describe no window; that state is under test.
    // NOLINTNEXTLINE(bugprone-use-after-move)
    CHECK(std::holds_alternative<std::monostate>(window.native()));
}
