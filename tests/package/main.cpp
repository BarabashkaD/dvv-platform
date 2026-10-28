#include <cstdio>
#include <string_view>

#include "dvv/platform/dynamic_library.h"
#include "dvv/platform/native_window.h"
#include "dvv/platform/surface.h"
#include "dvv/platform/testing/test_window.h"

int main() {
#if defined(_WIN32)
    const auto library = dvv::platform::DynamicLibrary::open(
        "kernel32.dll", dvv::platform::LibrarySearch::SystemDirectory);
    const char* const symbol = "GetTickCount64";
    constexpr std::string_view expected_system = "Win32";
#else
    const auto library = dvv::platform::DynamicLibrary::open("libc.so.6");
    const char* const symbol = "getpid";
    constexpr std::string_view expected_system = "Xlib";
#endif
    if (!library) {
        std::printf("%s\n", library.error().describe().c_str());
        return 1;
    }
    if (library->symbol(symbol) == nullptr) {
        std::printf("%s has no %s\n", library->name().c_str(), symbol);
        return 1;
    }
    const std::string_view system =
        dvv::platform::windowSystemName(dvv::platform::defaultWindowSystem());
    if (system != expected_system) {
        std::printf("the default window system is %.*s\n", static_cast<int>(system.size()),
                    system.data());
        return 1;
    }
    // Taking the addresses links both libraries without creating a window.
    auto* volatile create_surface = &dvv::platform::createSurface;
    auto* volatile create_window = &dvv::platform::testing::TestWindow::create;
    if (create_surface == nullptr || create_window == nullptr) {
        return 1;
    }
    std::puts("dvv_platform package works");
    return 0;
}
