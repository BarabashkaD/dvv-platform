#include <algorithm>
#include <climits>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>

#include <windows.h>

#include "dynamic_library_detail.h"

namespace dvv::platform::detail {

namespace {

// UTF-8 to UTF-16; nullopt for invalid UTF-8.
std::optional<std::wstring> widen(std::string_view text) {
    if (text.size() > static_cast<std::size_t>(INT_MAX)) {
        return std::nullopt;
    }
    const int length = static_cast<int>(text.size());
    const int size =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, nullptr, 0);
    if (size <= 0) {
        return std::nullopt;
    }
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, wide.data(), size);
    return wide;
}

bool isSlash(char c) noexcept {
    return c == '/' || c == '\\';
}

}  // namespace

bool isAbsolutePath(std::string_view path) noexcept {
    if (path.size() >= 2 && isSlash(path[0]) && isSlash(path[1])) {
        return true;  // UNC (\\server\share\...) or a device path (\\?\...)
    }
    const bool letter = path.size() >= 3 &&
                        ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z'));
    return letter && path[1] == ':' && isSlash(path[2]);
}

std::string win32ErrorMessage(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::string text;
    if (length > 0 && buffer != nullptr) {
        const int size = WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(length), nullptr,
                                             0, nullptr, nullptr);
        if (size > 0) {
            text.resize(static_cast<std::size_t>(size));
            WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(length), text.data(), size,
                                nullptr, nullptr);
        }
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
    const auto wide = widen(name);
    if (!wide) {
        error = "the name is not valid UTF-8";
        return nullptr;
    }
    std::wstring path = *wide;
    DWORD flags = 0;
    switch (search) {
        case LibrarySearch::Default: break;
        case LibrarySearch::SystemDirectory: flags = LOAD_LIBRARY_SEARCH_SYSTEM32; break;
        case LibrarySearch::DllDirectory:
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
