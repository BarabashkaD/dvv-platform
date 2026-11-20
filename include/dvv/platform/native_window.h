#pragma once

// Native windows as the platform layer sees them (spec §3.1, §10.4): plain handles, no toolkit.

#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>

#include "dvv/platform/pixel_size.h"

namespace dvv::platform {

enum class WindowSystem : std::uint8_t { Win32, Xlib, Metal };
[[nodiscard]] std::string_view windowSystemName(WindowSystem system) noexcept;
// "Win32", "Xlib", "Metal"
// The window system this build presents to: Win32 on Windows, Xlib on Linux, Metal on macOS.
[[nodiscard]] WindowSystem defaultWindowSystem() noexcept;

struct Win32Window {  // HINSTANCE, HWND
    void* hinstance = nullptr;
    void* hwnd = nullptr;
};
struct XlibWindow {  // Display*, XID
    void* display = nullptr;
    unsigned long window = 0;
};
struct MetalWindow {  // CAMetalLayer*
    void* layer = nullptr;
};
// The caller keeps the window (and, for Xlib, the display, opened after XInitThreads(); for Metal,
// the CAMetalLayer) alive for as long as any surface made from it exists.
using NativeWindow = std::variant<std::monostate, Win32Window, XlibWindow, MetalWindow>;
// nullopt for std::monostate.
[[nodiscard]] std::optional<WindowSystem> windowSystemOf(const NativeWindow& window) noexcept;

}  // namespace dvv::platform
