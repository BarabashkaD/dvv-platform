#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <string_view>

#include <windows.h>

#include "dvv/base/paths.h"
#include "dvv/base/utf8.h"
#include "dvv/base/utf8_win32.h"
#include "dynamic_library_detail.h"

namespace dvv::platform::detail {

namespace {

bool isSlash(char c) noexcept {
    return c == '/' || c == '\\';
}

}  // namespace

bool isAbsolutePath(std::string_view path) noexcept {
    if (path.size() >= 2 && isSlash(path[0]) && isSlash(path[1])) {
        return true;  // UNC (\\server\share\...) or a device path (\\?\...)
    }
    return path.size() >= 3 && dvv::isDriveLetterPath(path) && isSlash(path[2]);
}

std::string win32ErrorMessage(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::string text;
    if (length > 0 && buffer != nullptr) {
        text = dvv::utf8FromWide(std::wstring_view(buffer, length)).value_or("");
    }
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' ||
                             text.back() == '.')) {
        text.pop_back();
    }
    return std::format("{} (error {})", text.empty() ? "unknown error" : text, code);
}

void* loadLibrary(const std::string& name, LibrarySearch search, std::string& error) {
    if (!dvv::isValidUtf8(name)) {
        error = "the name is not valid UTF-8";
        return nullptr;
    }
    std::wstring path = dvv::wideFromUtf8(name);
    DWORD flags = 0;
    switch (search) {
        case LibrarySearch::Default: break;
        case LibrarySearch::SystemDirectory: flags = LOAD_LIBRARY_SEARCH_SYSTEM32; break;
        case LibrarySearch::LibraryDirectory:
            // LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR needs a fully qualified path with backslashes.
            std::ranges::replace(path, L'/', L'\\');
            flags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
                    LOAD_LIBRARY_SEARCH_SYSTEM32;
            break;
    }
    // No "missing DLL" dialog box while loading.
    DWORD old_mode = 0;
    const bool mode_set =
        SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &old_mode) != FALSE;
    HMODULE const module = LoadLibraryExW(path.c_str(), nullptr, flags);
    const DWORD code = module == nullptr ? GetLastError() : DWORD{0};
    if (mode_set) {
        SetThreadErrorMode(old_mode, nullptr);
    }
    if (module == nullptr) {
        error = win32ErrorMessage(code);
    }
    return module;
}

void* findSymbol(void* handle, const char* name) noexcept {
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle), name));
}

void unloadLibrary(void* handle) noexcept {
    FreeLibrary(static_cast<HMODULE>(handle));
}

}  // namespace dvv::platform::detail

namespace dvv::platform {

std::string libraryFileName(std::string_view name, unsigned major) {
    return std::format("{}-{}.dll", name, major);
}

}  // namespace dvv::platform
