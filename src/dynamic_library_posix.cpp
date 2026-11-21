#include <format>
#include <string>
#include <string_view>

#include <dlfcn.h>

#include "dynamic_library_detail.h"

namespace dvv::platform::detail {

bool isAbsolutePath(std::string_view path) noexcept {
    return path.starts_with('/');
}

// Every search, LibraryDirectory included, passes the name to dlopen as is.

void* loadLibrary(const std::string& name, LibrarySearch /*search*/, std::string& error) {
    void* const handle = dlopen(name.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        const char* const text = dlerror();
        error = text != nullptr ? text : "unknown error";
    }
    return handle;
}

void* findSymbol(void* handle, const char* name) noexcept {
    return dlsym(handle, name);
}

void unloadLibrary(void* handle) noexcept {
    dlclose(handle);
}

}  // namespace dvv::platform::detail

namespace dvv::platform {

std::string libraryFileName(std::string_view name, unsigned major) {
#if defined(__APPLE__)
    return std::format("lib{}.{}.dylib", name, major);  // macOS: the version sits before the suffix
#else
    return std::format("lib{}.so.{}", name, major);
#endif
}

}  // namespace dvv::platform
