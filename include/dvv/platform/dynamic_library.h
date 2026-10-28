#pragma once

// Run-time libraries (spec §10.4): a move-only RAII handle, exempt from the managed-object rules of
// spec §9.4 like other handles. The Vulkan loader is opened with LibrarySearch::SystemDirectory on
// Windows, so a vulkan-1.dll planted next to the executable or in the current directory is not
// loaded (unless a module with that name is already loaded in the process; Windows returns that
// one).

#include <cstdint>
#include <string>
#include <string_view>

#include "dvv/base/error.h"

namespace dvv::platform {

enum class LibrarySearch : std::uint8_t {
    Default,          // Windows LoadLibraryExW(name, nullptr, 0); Linux dlopen(RTLD_NOW|RTLD_LOCAL)
    SystemDirectory,  // Windows LOAD_LIBRARY_SEARCH_SYSTEM32 only (not app dir, cwd, PATH; an
                      // already loaded module of the same name is returned as is);
                      // Linux as Default. `name` must be a bare file name.
    // `name` is an absolute path (else Errc::Internal). Windows: LoadLibraryExW with
    // LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
    // LOAD_LIBRARY_SEARCH_SYSTEM32: the library's dependencies come only from its own folder, the
    // executable's folder and System32 ('/' is accepted and passed on as '\'). Linux: dlopen of the
    // path (RTLD_NOW | RTLD_LOCAL); dependencies through the library's RUNPATH or sonames already
    // loaded.
    DllDirectory,
};

// Move-only RAII handle (spec §9.4 "handles"); the destructor unloads unless keepLoaded().
class DynamicLibrary {
public:
    // UTF-8 name. Errc::Unsupported "'<name>' could not be loaded: <system message>";
    // Errc::Internal for an empty name, a name containing '\0', a SystemDirectory name
    // containing '/' or '\\', or a DllDirectory name that is not an absolute path
    // ("DynamicLibrary::open: '<name>' must be an absolute path with LibrarySearch::DllDirectory").
    [[nodiscard]] static Result<DynamicLibrary> open(std::string_view name,
                                                     LibrarySearch search = LibrarySearch::Default);
    DynamicLibrary(DynamicLibrary&& other) noexcept;
    DynamicLibrary& operator=(DynamicLibrary&& other) noexcept;
    DynamicLibrary(const DynamicLibrary&) = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;
    ~DynamicLibrary();

    [[nodiscard]] void* symbol(const char* name) const noexcept;  // nullptr when missing or empty
    template <class Function>
    [[nodiscard]] Function function(const char* name) const noexcept {
        return reinterpret_cast<Function>(symbol(name));
    }
    void keepLoaded() noexcept;  // never unloaded, also after destruction (the Vulkan loader)
    [[nodiscard]] const std::string& name() const noexcept { return name_; }

private:
    DynamicLibrary(void* handle, std::string name) noexcept;
    void release() noexcept;  // unloads unless empty or kept loaded

    void* handle_ = nullptr;
    std::string name_;
    bool keep_loaded_ = false;
};

}  // namespace dvv::platform
