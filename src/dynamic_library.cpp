#include "dvv/platform/dynamic_library.h"

#include <format>
#include <string>
#include <string_view>
#include <utility>

#include "dynamic_library_detail.h"

namespace dvv::platform {

Result<DynamicLibrary> DynamicLibrary::open(std::string_view name, LibrarySearch search) {
    if (name.empty() || name.find('\0') != std::string_view::npos) {
        return makeError(Errc::Internal,
                         "DynamicLibrary::open: the library name is empty or contains a null "
                         "character");
    }
    if (search == LibrarySearch::SystemDirectory &&
        name.find_first_of("/\\") != std::string_view::npos) {
        return makeError(Errc::Internal,
                         std::format("DynamicLibrary::open: '{}' must be a bare file name with "
                                     "LibrarySearch::SystemDirectory",
                                     name));
    }
    if (search == LibrarySearch::DllDirectory && !detail::isAbsolutePath(name)) {
        return makeError(Errc::Internal,
                         std::format("DynamicLibrary::open: '{}' must be an absolute path with "
                                     "LibrarySearch::DllDirectory",
                                     name));
    }
    std::string owned(name);
    std::string error;
    void* const handle = detail::loadLibrary(owned, search, error);
    if (handle == nullptr) {
        return makeError(Errc::Unsupported,
                         std::format("'{}' could not be loaded: {}", owned, error));
    }
    return DynamicLibrary(handle, std::move(owned));
}

DynamicLibrary::DynamicLibrary(void* handle, std::string name) noexcept
    : handle_(handle), name_(std::move(name)) {}

DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)),
      name_(std::exchange(other.name_, {})),
      keep_loaded_(std::exchange(other.keep_loaded_, false)) {}

DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept {
    if (this != &other) {
        release();
        handle_ = std::exchange(other.handle_, nullptr);
        name_ = std::exchange(other.name_, {});
        keep_loaded_ = std::exchange(other.keep_loaded_, false);
    }
    return *this;
}

DynamicLibrary::~DynamicLibrary() {
    release();
}

void* DynamicLibrary::symbol(const char* name) const noexcept {
    return handle_ != nullptr ? detail::findSymbol(handle_, name) : nullptr;
}

void DynamicLibrary::keepLoaded() noexcept {
    keep_loaded_ = true;
}

void DynamicLibrary::release() noexcept {
    if (handle_ != nullptr && !keep_loaded_) {
        detail::unloadLibrary(handle_);
    }
    handle_ = nullptr;
}

}  // namespace dvv::platform
