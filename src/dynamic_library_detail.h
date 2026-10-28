#pragma once

// The platform part of DynamicLibrary, implemented once per platform (dynamic_library_win32.cpp,
// dynamic_library_posix.cpp). The checks, moves and unloading rules live in dynamic_library.cpp.

#include <string>
#include <string_view>

#include "dvv/platform/dynamic_library.h"

namespace dvv::platform::detail {

// Whether a UTF-8 path is absolute, lexically (no file-system access). Windows: "X:\..." or
// "X:/..." (an ASCII letter, ':', a slash) or a UNC path (two leading slashes of either kind); a
// drive-relative "C:x.dll" is not absolute. Linux: a leading '/'.
[[nodiscard]] bool isAbsolutePath(std::string_view path) noexcept;

// Loads a library by its UTF-8 name; returns its handle, or nullptr with `error` set to the
// system's message.
[[nodiscard]] void* loadLibrary(const std::string& name, LibrarySearch search, std::string& error);

// The address of an exported symbol, or nullptr when it is missing.
[[nodiscard]] void* findSymbol(void* handle, const char* name) noexcept;

// Releases one reference to a handle that loadLibrary returned.
void unloadLibrary(void* handle) noexcept;

#if defined(_WIN32)
// The system's text for a GetLastError() code (a DWORD), in UTF-8, with the code:
// "<text> (error <code>)". Shared with FileReader's systemMessage().
[[nodiscard]] std::string win32ErrorMessage(unsigned long code);
#endif

}  // namespace dvv::platform::detail
