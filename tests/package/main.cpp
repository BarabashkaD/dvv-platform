#include <cstdio>
#include <string_view>

#include "dvv/platform/aligned_buffer.h"
#include "dvv/platform/crash_handler.h"
#include "dvv/platform/dynamic_library.h"
#include "dvv/platform/file_writer.h"
#include "dvv/platform/pixel_size.h"
#include "dvv/platform/testing/test_window.h"
#include "dvv/platform/window_handle.h"
#include "dvv/platform/window_handle_metal.h"
#include "dvv/platform/window_handle_win32.h"
#include "dvv/platform/window_handle_xlib.h"

// dvv::platform carries no Vulkan definition any more.
#if defined(VK_NO_PROTOTYPES)
#error "dvv::platform still defines VK_NO_PROTOTYPES"
#endif

// Defined in window_handle_only.cpp, which includes window_handle.h and nothing else.
bool dvvPlatformHandleIsEmpty();

int main() {
    if (!dvvPlatformHandleIsEmpty()) {
        return 1;
    }
#if defined(_WIN32)
    const auto library = dvv::platform::DynamicLibrary::open(
        "kernel32.dll", dvv::platform::LibrarySearch::SystemDirectory);
    const char* const symbol = "GetTickCount64";
    constexpr std::string_view expected_system = "Win32";
#elif defined(__APPLE__)
    const auto library = dvv::platform::DynamicLibrary::open("libSystem.B.dylib");
    const char* const symbol = "getpid";
    constexpr std::string_view expected_system = "Metal";
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
    auto* volatile create_window = &dvv::platform::testing::TestWindow::create;
    auto* volatile replace_file = &dvv::platform::replaceFile;
    auto* volatile library_file_name = &dvv::platform::libraryFileName;
    auto* volatile install_crash_handler = &dvv::platform::installCrashHandler;
    if (create_window == nullptr || replace_file == nullptr || library_file_name == nullptr ||
        install_crash_handler == nullptr) {
        return 1;
    }
    // The new public types are named, so a header that stops declaring one fails here.
    const dvv::platform::WindowHandle handle;
    const dvv::platform::PixelSize size;
    const dvv::platform::AlignedBuffer buffer;
    const dvv::platform::ReplaceFileOptions replace_options;
    const dvv::platform::CrashHandlerOptions crash_options;
    if (!handle.empty() || !size.empty() || buffer != nullptr || replace_options.retries != 0 ||
        !crash_options.app.empty()) {
        return 1;
    }
    std::puts("dvv_platform package works");
    return 0;
}
