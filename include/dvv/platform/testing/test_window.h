#pragma once

// Target dvv::platform_testing; for tests only.

#include <memory>
#include <string_view>

#include "dvv/base/error.h"
#include "dvv/platform/native_window.h"

namespace dvv::platform::testing {

// A visible top-level window for display tests: CreateWindowExW, or XCreateSimpleWindow on its own
// Display (XInitThreads() once before the first XOpenDisplay). Move-only; every member is called on
// the thread that created it. Destroy every surface and presenter made from it first.
class TestWindow {
public:
    // Errc::Unsupported "no display ..." (Linux without DISPLAY, or the display cannot be opened);
    // Errc::Internal for an empty size or when the window cannot be created or mapped.
    [[nodiscard]] static Result<TestWindow> create(PixelSize client_size,
                                                   std::string_view title = "dvv test window");
    TestWindow(TestWindow&& other) noexcept;
    TestWindow& operator=(TestWindow&& other) noexcept;
    TestWindow(const TestWindow&) = delete;
    TestWindow& operator=(const TestWindow&) = delete;
    ~TestWindow();

    [[nodiscard]] NativeWindow native() const noexcept;  // std::monostate when moved from
    [[nodiscard]] PixelSize clientSize() const;          // as the window system reports it
    void resize(PixelSize client_size);  // synchronous (SetWindowPos / XResizeWindow + XSync)
    void minimise();  // Win32 SW_MINIMIZE; Xlib unmap (no window manager under Xvfb)
    void restore();   // Win32 SW_RESTORE; Xlib map
    void pump();      // Win32 dispatches pending messages; Xlib XSync and drops queued events

private:
    struct State;
    explicit TestWindow(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> state_;
};

}  // namespace dvv::platform::testing
