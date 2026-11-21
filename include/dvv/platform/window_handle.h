#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>

namespace dvv::platform {

enum class WindowSystem : std::uint8_t { Win32, Xlib, Metal };
[[nodiscard]] std::string_view windowSystemName(WindowSystem system) noexcept;  // "Win32", …
// The window system this build presents to: Win32 on Windows, Xlib on Linux, Metal on macOS.
[[nodiscard]] WindowSystem defaultWindowSystem() noexcept;

// A native window as the platform layer sees it: a window system and two native words, trivially
// copyable, empty by default. Build and read it through the per-OS header of its system
// (window_handle_win32.h, window_handle_xlib.h, window_handle_metal.h). The caller keeps the
// window (for Xlib also the Display, opened after XInitThreads(); for Metal the CAMetalLayer)
// alive while anything made from the handle exists.
class WindowHandle {
public:
    using Words = std::array<std::uintptr_t, 2>;

    constexpr WindowHandle() noexcept = default;
    // Any system on any OS: the per-OS headers and cross-OS tests use it.
    [[nodiscard]] static constexpr WindowHandle make(WindowSystem system, std::uintptr_t first,
                                                     std::uintptr_t second) noexcept {
        WindowHandle handle;
        handle.words_ = {first, second};
        handle.tag_ = static_cast<std::uint8_t>(static_cast<unsigned>(system) + 1U);
        return handle;
    }
    [[nodiscard]] constexpr bool empty() const noexcept { return tag_ == 0; }
    [[nodiscard]] constexpr std::optional<WindowSystem> system() const noexcept {
        if (tag_ == 0) {
            return std::nullopt;
        }
        return static_cast<WindowSystem>(tag_ - 1U);
    }
    // The native words; their meaning belongs to the per-OS header of system().
    [[nodiscard]] constexpr Words words() const noexcept { return words_; }
    friend constexpr bool operator==(const WindowHandle&, const WindowHandle&) noexcept = default;

private:
    Words words_{};
    std::uint8_t tag_ = 0;  // 0: empty; else 1 + WindowSystem. Not part of the API.
};
static_assert(std::is_trivially_copyable_v<WindowHandle>);

}  // namespace dvv::platform
