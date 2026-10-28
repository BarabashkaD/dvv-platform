#include <cstddef>
#include <format>
#include <string>
#include <utility>

#include <windows.h>

#include "dvv/platform/testing/test_window.h"

namespace dvv::platform::testing {

// Owns the window: destroying the state destroys it (also when a TestWindow is assigned over).
struct TestWindow::State {
    State() = default;
    State(const State&) = delete;
    State& operator=(const State&) = delete;
    ~State() {
        if (hwnd != nullptr) {
            DestroyWindow(hwnd);
        }
    }

    HINSTANCE instance = nullptr;
    HWND hwnd = nullptr;
};

namespace {

constexpr wchar_t CLASS_NAME[] = L"dvvTestWindow";
// No WS_SYSMENU: with the caption buttons the system keeps the window at least SM_CXMINTRACK wide
// whatever WM_GETMINMAXINFO says, which would stop tests from using narrow client sizes.
constexpr DWORD STYLE = WS_OVERLAPPEDWINDOW & ~static_cast<DWORD>(WS_SYSMENU);
constexpr DWORD EX_STYLE = 0;

LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_GETMINMAXINFO) {
        // Tests use client sizes far below the default minimum tracking size.
        reinterpret_cast<MINMAXINFO*>(lparam)->ptMinTrackSize = {1, 1};
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

// Once per process: per-monitor DPI awareness (v2), so sizes are physical pixels on every thread,
// and the window class. False when the class cannot be registered.
bool prepareProcess(HINSTANCE instance) {
    static const bool READY = [instance] {
        // Fails when the awareness was set already (e.g. by a manifest); either way sizes are
        // physical then.
        static_cast<void>(
            SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2));
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = &windowProc;
        window_class.hInstance = instance;
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.lpszClassName = CLASS_NAME;
        return RegisterClassExW(&window_class) != 0;
    }();
    return READY;
}

std::wstring widen(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = static_cast<int>(text.size());
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), length, nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size > 0 ? size : 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), length, wide.data(), size);
    return wide;
}

}  // namespace

Result<TestWindow> TestWindow::create(PixelSize client_size, std::string_view title) {
    if (client_size.empty()) {
        return makeError(Errc::Internal, "TestWindow::create: the client size is empty");
    }
    HINSTANCE const instance = GetModuleHandleW(nullptr);
    if (!prepareProcess(instance)) {
        return makeError(Errc::Internal, "TestWindow::create: RegisterClassExW failed");
    }
    HWND const hwnd = CreateWindowExW(EX_STYLE, CLASS_NAME, widen(title).c_str(), STYLE,
                                      CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                      nullptr, nullptr, instance, nullptr);
    if (hwnd == nullptr) {
        return makeError(Errc::Internal,
                         std::format("CreateWindowExW failed (error {})", GetLastError()));
    }
    auto state = std::make_unique<State>();
    state->instance = instance;
    state->hwnd = hwnd;
    TestWindow window(std::move(state));
    window.resize(client_size);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    window.pump();
    return window;
}

TestWindow::TestWindow(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}
TestWindow::TestWindow(TestWindow&& other) noexcept = default;
TestWindow& TestWindow::operator=(TestWindow&& other) noexcept = default;
TestWindow::~TestWindow() = default;

NativeWindow TestWindow::native() const noexcept {
    if (!state_) {
        return std::monostate{};
    }
    return Win32Window{.hinstance = state_->instance, .hwnd = state_->hwnd};
}

PixelSize TestWindow::clientSize() const {
    RECT rect{};
    GetClientRect(state_->hwnd, &rect);
    return {.width = static_cast<std::uint32_t>(rect.right > 0 ? rect.right : 0),
            .height = static_cast<std::uint32_t>(rect.bottom > 0 ? rect.bottom : 0)};
}

void TestWindow::resize(PixelSize client_size) {
    HWND const hwnd = state_->hwnd;
    RECT rect{0, 0, static_cast<LONG>(client_size.width), static_cast<LONG>(client_size.height)};
    AdjustWindowRectExForDpi(&rect, STYLE, FALSE, EX_STYLE, GetDpiForWindow(hwnd));
    SetWindowPos(hwnd, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    const PixelSize actual = clientSize();
    if (actual != client_size && IsIconic(hwnd) == FALSE) {
        // The frame differs from what AdjustWindowRectExForDpi assumed: correct it once.
        RECT outer{};
        GetWindowRect(hwnd, &outer);
        const LONG width = (outer.right - outer.left) + static_cast<LONG>(client_size.width) -
                           static_cast<LONG>(actual.width);
        const LONG height = (outer.bottom - outer.top) + static_cast<LONG>(client_size.height) -
                            static_cast<LONG>(actual.height);
        SetWindowPos(hwnd, nullptr, 0, 0, width, height,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    pump();
}

void TestWindow::minimise() {
    ShowWindow(state_->hwnd, SW_MINIMIZE);
    pump();
}

void TestWindow::restore() {
    ShowWindow(state_->hwnd, SW_RESTORE);
    pump();
}

void TestWindow::pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

}  // namespace dvv::platform::testing
