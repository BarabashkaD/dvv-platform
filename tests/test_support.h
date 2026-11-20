#pragma once

// Helpers shared by the unit tests: a temporary directory and a logger scope.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <format>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "dvv/base/log.h"

namespace dvv::platform::test {

// A unique directory "<prefix>-<16 hex digits>" under the system temporary directory, removed with
// its contents.
class TemporaryDirectory {
public:
    explicit TemporaryDirectory(std::string_view prefix = "dvv-file-reader") {
        std::random_device random;
        const std::filesystem::path base = std::filesystem::temp_directory_path();
        for (int attempt = 0; attempt < 100; ++attempt) {
            std::filesystem::path candidate =
                base / std::format("{}-{:08x}{:08x}", prefix, random(), random());
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                path_ = std::move(candidate);
                return;
            }
        }
        FAIL("no temporary directory could be created");
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    TemporaryDirectory(TemporaryDirectory&&) = delete;
    TemporaryDirectory& operator=(TemporaryDirectory&&) = delete;
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

// Restores the logger's level and removes the test's sinks (the tests add no other sinks).
class LoggerScope {
public:
    LoggerScope() : level_(dvv::Logger::instance().level()) {
        dvv::Logger::instance().clearSinks();
        dvv::Logger::instance().addSink(sink_);
        dvv::Logger::instance().setLevel(dvv::LogLevel::Info);
    }
    LoggerScope(const LoggerScope&) = delete;
    LoggerScope& operator=(const LoggerScope&) = delete;
    LoggerScope(LoggerScope&&) = delete;
    LoggerScope& operator=(LoggerScope&&) = delete;
    ~LoggerScope() {
        dvv::Logger::instance().clearSinks();
        dvv::Logger::instance().setLevel(level_);
    }

    [[nodiscard]] std::size_t count(std::string_view text) const {
        const std::vector<std::string> lines = sink_->lines();
        return static_cast<std::size_t>(std::ranges::count_if(
            lines, [text](const std::string& line) { return line.contains(text); }));
    }

private:
    dvv::LogLevel level_;
    std::shared_ptr<dvv::MemorySink> sink_ = std::make_shared<dvv::MemorySink>();
};

}  // namespace dvv::platform::test
