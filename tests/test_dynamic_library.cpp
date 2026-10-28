#include <doctest/doctest.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dvv/base/error.h"
#include "dvv/platform/dynamic_library.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

using dvv::platform::DynamicLibrary;
using dvv::platform::LibrarySearch;
using TestFunction = int (*)();

// The error of an open() that must fail.
dvv::Error openError(std::string_view name, LibrarySearch search = LibrarySearch::Default) {
    auto library = DynamicLibrary::open(name, search);
    REQUIRE_FALSE(library.has_value());
    return std::move(library).error();
}

// Whether the test module is loaded in this process, without loading it.
bool testModuleLoaded() {
#if defined(_WIN32)
    // The loader matches the base name against loaded modules; DVV_PLATFORM_TEST_MODULE has forward
    // slashes.
    const std::wstring name = std::filesystem::path(DVV_PLATFORM_TEST_MODULE_NAME).native();
    return GetModuleHandleW(name.c_str()) != nullptr;
#else
    // RTLD_NOLOAD takes another reference when the library is loaded; give it back.
    void* const handle = dlopen(DVV_PLATFORM_TEST_MODULE, RTLD_NOW | RTLD_NOLOAD);
    if (handle == nullptr) {
        return false;
    }
    dlclose(handle);
    return true;
#endif
}

}  // namespace

TEST_CASE("a library opened by its path gives its functions") {
    const auto library = DynamicLibrary::open(DVV_PLATFORM_TEST_MODULE);
    REQUIRE(library.has_value());
    CHECK(library->name() == DVV_PLATFORM_TEST_MODULE);
    const auto function = library->function<TestFunction>("dvvTestValue");
    REQUIRE(function != nullptr);
    CHECK(function() == 42);
    CHECK(library->symbol("dvvTestValue") != nullptr);
}

TEST_CASE("a missing symbol is null") {
    const auto library = DynamicLibrary::open(DVV_PLATFORM_TEST_MODULE);
    REQUIRE(library.has_value());
    CHECK(library->symbol("dvvNoSuchSymbol") == nullptr);
    CHECK(library->function<TestFunction>("dvvNoSuchSymbol") == nullptr);
}

TEST_CASE("a missing library is unsupported") {
    const dvv::Error error = openError("dvv-test-no-such-library");
    CHECK(error.code == dvv::Errc::Unsupported);
    const std::string_view prefix = "'dvv-test-no-such-library' could not be loaded: ";
    CHECK(error.message.starts_with(prefix));
    CHECK(error.message.size() > prefix.size());  // the system's text follows
}

TEST_CASE("an empty name and a path with SystemDirectory are internal errors") {
    CHECK(openError("").code == dvv::Errc::Internal);
    CHECK(openError("", LibrarySearch::SystemDirectory).code == dvv::Errc::Internal);
    CHECK(openError(std::string_view("a\0b", 3)).code == dvv::Errc::Internal);
    for (const std::string_view path : {"sub/x.dll", "sub\\x.dll"}) {
        CAPTURE(path);
        const dvv::Error error = openError(path, LibrarySearch::SystemDirectory);
        CHECK(error.code == dvv::Errc::Internal);
        CHECK(error.message.find("must be a bare file name") != std::string::npos);
    }
    // The default search accepts paths; this one does not exist.
    CHECK(openError("sub/x.dll").code == dvv::Errc::Unsupported);
#if defined(_WIN32)
    {
        const dvv::Error invalid = openError("\xff.dll");
        CHECK(invalid.code == dvv::Errc::Unsupported);
        CHECK(invalid.message.find("not valid UTF-8") != std::string::npos);
    }
#endif
}

#if defined(_WIN32)
TEST_CASE("SystemDirectory loads system libraries but not the application's") {
    std::wstring executable(32768, L'\0');
    const DWORD length =
        GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    REQUIRE(length > 0);
    REQUIRE(length < executable.size());
    executable.resize(length);
    const std::filesystem::path module =
        std::filesystem::path(executable).parent_path() / DVV_PLATFORM_TEST_MODULE_NAME;
    REQUIRE(std::filesystem::exists(module));
    REQUIRE_FALSE(testModuleLoaded());  // a loaded module of this name would satisfy the search

    CHECK(openError(DVV_PLATFORM_TEST_MODULE_NAME, LibrarySearch::SystemDirectory).code ==
          dvv::Errc::Unsupported);
    // The default search includes the application directory.
    CHECK(DynamicLibrary::open(DVV_PLATFORM_TEST_MODULE_NAME).has_value());

    const auto kernel32 = DynamicLibrary::open("kernel32.dll", LibrarySearch::SystemDirectory);
    REQUIRE(kernel32.has_value());
    CHECK(kernel32->symbol("GetTickCount64") != nullptr);
}
#endif

TEST_CASE("a moved library leaves an empty handle behind") {
    auto opened = DynamicLibrary::open(DVV_PLATFORM_TEST_MODULE);
    REQUIRE(opened.has_value());
    DynamicLibrary moved = std::move(*opened);
    CHECK(moved.symbol("dvvTestValue") != nullptr);
    CHECK(opened->symbol("dvvTestValue") == nullptr);
    CHECK(opened->name().empty());

    auto second = DynamicLibrary::open(DVV_PLATFORM_TEST_MODULE);
    REQUIRE(second.has_value());
    moved = std::move(*second);
    const auto function = moved.function<TestFunction>("dvvTestValue");
    REQUIRE(function != nullptr);
    CHECK(function() == 42);
}

#if defined(_WIN32)
TEST_CASE("a destroyed library is unloaded") {
    {
        const auto library = DynamicLibrary::open(DVV_PLATFORM_TEST_MODULE);
        REQUIRE(library.has_value());
        CHECK(testModuleLoaded());
    }
    CHECK_FALSE(testModuleLoaded());
}
#endif

TEST_CASE("DllDirectory finds a dependency in the library's own folder") {
#if defined(_WIN32)
    // The default search does not look in the library's folder, so its dependency is missing. This
    // also proves that the dependency is not loaded yet, which would satisfy any search.
    CHECK(openError(DVV_PLATFORM_TEST_DEPENDENT).code == dvv::Errc::Unsupported);
#endif
    const auto library =
        DynamicLibrary::open(DVV_PLATFORM_TEST_DEPENDENT, LibrarySearch::DllDirectory);
    REQUIRE(library.has_value());
    CHECK(library->name() == DVV_PLATFORM_TEST_DEPENDENT);
    const auto function = library->function<TestFunction>("dvvTestDependentValue");
    REQUIRE(function != nullptr);
    CHECK(function() == 43);
}

TEST_CASE("DllDirectory takes absolute paths only") {
    std::vector<std::string_view> relative{"dll-directory/x.dll", DVV_PLATFORM_TEST_MODULE_NAME,
                                           "x"};
#if defined(_WIN32)
    relative.emplace_back("C:x.dll");  // drive-relative
#endif
    for (const std::string_view name : relative) {
        CAPTURE(name);
        const dvv::Error error = openError(name, LibrarySearch::DllDirectory);
        CHECK(error.code == dvv::Errc::Internal);
        CHECK(error.message.find("must be an absolute path") != std::string::npos);
    }

    const std::u8string missing_u8 =
        (std::filesystem::path(DVV_PLATFORM_TEST_DEPENDENT).parent_path() /
         "dvv-no-such-library.dll")
            .generic_u8string();
    const std::string missing(missing_u8.begin(), missing_u8.end());
    const dvv::Error error = openError(missing, LibrarySearch::DllDirectory);
    CHECK(error.code == dvv::Errc::Unsupported);
    const std::string prefix = "'" + missing + "' could not be loaded: ";
    CHECK(error.message.starts_with(prefix));
    CHECK(error.message.size() > prefix.size());  // the system's text follows
}

// Keep this case last: a kept library is never unloaded, so later cases could not check unloading.
TEST_CASE("keepLoaded keeps the library loaded after destruction") {
    TestFunction function = nullptr;
    {
        auto library = DynamicLibrary::open(DVV_PLATFORM_TEST_MODULE);
        REQUIRE(library.has_value());
        function = library->function<TestFunction>("dvvTestValue");
        REQUIRE(function != nullptr);
        library->keepLoaded();
    }
    CHECK(testModuleLoaded());
    CHECK(function() == 42);
}
