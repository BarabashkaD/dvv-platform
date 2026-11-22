#include <doctest/doctest.h>

#include <utility>

#include "dvv/platform/testing/test_window.h"
#include "dvv/platform/window_handle_win32.h"

using dvv::platform::PixelSize;
using dvv::platform::testing::TestWindow;

TEST_CASE("a test window reports, changes and restores its client size") {
    auto created = TestWindow::create({.width = 320, .height = 240});
    REQUIRE(created.has_value());
    TestWindow window = std::move(*created);
    CHECK(window.clientSize() == PixelSize{.width = 320, .height = 240});
    CHECK(window.handle().system() == dvv::platform::defaultWindowSystem());
    // Word 0 is the display / instance (the layer on Metal); word 1 is the window itself.
    CHECK(window.handle().words()[0] != 0);
#if defined(_WIN32)
    const auto win32_window = dvv::platform::win32WindowOf(window.handle());
    REQUIRE(win32_window.has_value());
    CHECK(win32_window->hwnd != nullptr);
#elif !defined(__APPLE__)
    CHECK(window.handle().words()[1] != 0);
#endif

    window.resize({.width = 200, .height = 150});
    CHECK(window.clientSize() == PixelSize{.width = 200, .height = 150});

    // Narrower than a default window can be (the smallest size dvv-gpu tests use).
    window.resize({.width = 97, .height = 61});
    CHECK(window.clientSize() == PixelSize{.width = 97, .height = 61});
    window.resize({.width = 200, .height = 150});
    CHECK(window.clientSize() == PixelSize{.width = 200, .height = 150});

    // Odd sizes: on a 2x display the window itself snaps to whole points, the client size does not.
    window.resize({.width = 333, .height = 177});
    CHECK(window.clientSize() == PixelSize{.width = 333, .height = 177});
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
    CHECK(window.handle().empty());
}
