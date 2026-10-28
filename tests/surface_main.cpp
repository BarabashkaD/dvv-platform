// Surface tests (CTest dvv_platform.surface, label display). Without a Vulkan loader, a driver that
// offers the surface extensions, or a display, the executable exits with 77 (CTest: skipped),
// unless DVV_REQUIRE_GPU=1 turns that into a failure (Linux CI under Xvfb).
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <cstdio>
#include <exception>
#include <expected>
#include <optional>
#include <print>
#include <string>

#include "dvv/base/environment.h"
#include "dvv/base/leak_check.h"
#include "dvv/base/lifetime.h"
#include "dvv/platform/surface.h"
#include "dvv/platform/testing/test_window.h"
#include "surface_support.h"

namespace {

constexpr int EXIT_SKIPPED = 77;

// nullopt when the tests can run; the reason when something they need is missing
// (Errc::Unsupported); an error for any other failure.
dvv::Result<std::optional<std::string>> missingRequirement() {
    const auto instance = dvv::platform::test::VulkanInstance::create(
        dvv::platform::surfaceInstanceExtensions(dvv::platform::defaultWindowSystem()));
    if (!instance) {
        if (instance.error().code != dvv::Errc::Unsupported) {
            return std::unexpected(instance.error());
        }
        return instance.error().describe();
    }
    const auto window = dvv::platform::testing::TestWindow::create({.width = 64, .height = 64});
    if (!window) {
        if (window.error().code != dvv::Errc::Unsupported) {
            return std::unexpected(window.error());
        }
        return window.error().describe();
    }
    return std::nullopt;
}

int run(int argc, char** argv) {
    dvv::LifetimeRegistry::instance().setDetailed(true);
    const auto missing = missingRequirement();
    if (!missing) {
        std::println(stderr, "surface tests failed: {}", missing.error().describe());
        return 1;
    }
    if (const std::optional<std::string>& reason = *missing; reason) {
        if (dvv::getEnvironment("DVV_REQUIRE_GPU") == "1") {
            std::println(stderr, "surface tests failed: DVV_REQUIRE_GPU=1, but {}", *reason);
            return 1;
        }
        std::println("surface tests skipped: {}", *reason);
        return EXIT_SKIPPED;
    }
    doctest::Context context(argc, argv);
    const int result = context.run();
    if (context.shouldExit()) {
        return result;
    }
    return dvv::finishWithLeakCheck(result);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "surface tests failed: %s\n", error.what());
        return 1;
    }
}
