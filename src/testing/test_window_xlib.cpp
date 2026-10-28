#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>

// X11 defines macros such as None, Bool, Status, Success, True and False; clang-format puts it
// before the dvv headers, which use none of those names (the standard headers they need come
// first).
#include <X11/Xlib.h>

#include "dvv/base/environment.h"
#include "dvv/platform/testing/test_window.h"

namespace dvv::platform::testing {

// Owns the window and its display connection: destroying the state destroys both (also when a
// TestWindow is assigned over).
struct TestWindow::State {
    State() = default;
    State(const State&) = delete;
    State& operator=(const State&) = delete;
    ~State() {
        if (display == nullptr) {
            return;
        }
        if (window != 0) {
            XDestroyWindow(display, window);
        }
        XCloseDisplay(display);
    }

    Display* display = nullptr;
    Window window = 0;
};

namespace {

// Polls `done` after XSync in 10 ms steps for up to 2 s; true once it holds.
template <class Done>
bool waitUntil(Display* display, Done done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    for (;;) {
        XSync(display, False);
        if (done()) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

XWindowAttributes attributes(Display* display, Window window) {
    XWindowAttributes result{};
    XGetWindowAttributes(display, window, &result);
    return result;
}

bool viewable(Display* display, Window window) {
    return attributes(display, window).map_state == IsViewable;
}

}  // namespace

Result<TestWindow> TestWindow::create(PixelSize client_size, std::string_view title) {
    if (client_size.empty()) {
        return makeError(Errc::Internal, "TestWindow::create: the client size is empty");
    }
    const auto name = getEnvironment("DISPLAY");
    if (!name || name->empty()) {
        return makeError(Errc::Unsupported, "no display: DISPLAY is not set");
    }
    // The Vulkan driver uses the display from the presenter thread as well.
    static std::once_flag threads;
    std::call_once(threads, [] { XInitThreads(); });
    Display* const display = XOpenDisplay(nullptr);
    if (display == nullptr) {
        return makeError(Errc::Unsupported, std::format("no display: cannot open '{}'", *name));
    }
    auto state = std::make_unique<State>();
    state->display = display;
    const int screen = DefaultScreen(display);
    const Window window = XCreateSimpleWindow(
        display, RootWindow(display, screen), 0, 0, client_size.width, client_size.height, 0,
        BlackPixel(display, screen), BlackPixel(display, screen));
    state->window = window;
    TestWindow result(std::move(state));
    const std::string owned_title(title);
    XStoreName(display, window, owned_title.c_str());
    XMapWindow(display, window);
    if (!waitUntil(display, [&] { return viewable(display, window); })) {
        return makeError(Errc::Internal, "the test window was not mapped within 2 s");
    }
    return result;
}

TestWindow::TestWindow(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}
TestWindow::TestWindow(TestWindow&& other) noexcept = default;
TestWindow& TestWindow::operator=(TestWindow&& other) noexcept = default;
TestWindow::~TestWindow() = default;

NativeWindow TestWindow::native() const noexcept {
    if (!state_) {
        return std::monostate{};
    }
    return XlibWindow{.display = state_->display, .window = state_->window};
}

PixelSize TestWindow::clientSize() const {
    const XWindowAttributes current = attributes(state_->display, state_->window);
    return {.width = static_cast<std::uint32_t>(current.width > 0 ? current.width : 0),
            .height = static_cast<std::uint32_t>(current.height > 0 ? current.height : 0)};
}

void TestWindow::resize(PixelSize client_size) {
    XResizeWindow(state_->display, state_->window, client_size.width, client_size.height);
    // A window manager applies the size asynchronously. No failure when it does not: the caller
    // compares clientSize().
    static_cast<void>(waitUntil(state_->display, [&] { return clientSize() == client_size; }));
}

void TestWindow::minimise() {
    XUnmapWindow(state_->display, state_->window);
    static_cast<void>(
        waitUntil(state_->display, [&] { return !viewable(state_->display, state_->window); }));
}

void TestWindow::restore() {
    XMapWindow(state_->display, state_->window);
    static_cast<void>(
        waitUntil(state_->display, [&] { return viewable(state_->display, state_->window); }));
}

void TestWindow::pump() {
    XSync(state_->display, False);
    // No input is selected, so the queue stays small.
    XEvent event{};
    while (XPending(state_->display) > 0) {
        XNextEvent(state_->display, &event);
    }
}

}  // namespace dvv::platform::testing
