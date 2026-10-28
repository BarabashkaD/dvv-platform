#pragma once

// Native windows as the platform layer sees them (spec §3.1, §10.4): plain handles, no toolkit.

#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>

namespace dvv::platform {

enum class WindowSystem : std::uint8_t { Win32, Xlib };
[[nodiscard]] std::string_view windowSystemName(WindowSystem system) noexcept;  // "Win32", "Xlib"
// The window system this build presents to: Win32 on Windows, Xlib on Linux.
[[nodiscard]] WindowSystem defaultWindowSystem() noexcept;

struct PixelSize {  // physical pixels
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    [[nodiscard]] constexpr bool empty() const noexcept { return width == 0 || height == 0; }
    friend constexpr bool operator==(PixelSize, PixelSize) noexcept = default;
};
struct Win32Window {  // HINSTANCE, HWND
    void* hinstance = nullptr;
    void* hwnd = nullptr;
};
struct XlibWindow {  // Display*, XID
    void* display = nullptr;
    unsigned long window = 0;
};
// The caller keeps the window (and, for Xlib, the display, opened after XInitThreads()) alive for
// as long as any surface made from it exists.
using NativeWindow = std::variant<std::monostate, Win32Window, XlibWindow>;
// nullopt for std::monostate.
[[nodiscard]] std::optional<WindowSystem> windowSystemOf(const NativeWindow& window) noexcept;

}  // namespace dvv::platform
