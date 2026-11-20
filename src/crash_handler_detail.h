// src/crash_handler_detail.h (private; dvv_platform_tests include src/)
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include "dvv/base/log.h"

namespace dvv::platform::detail {
struct CrashBackendSetup {
    std::filesystem::path folder;  // <userDataDir(app)>/crashes; its parents are made at a crash
    std::string app;
    std::string process;
    const dvv::Logger* logger = nullptr;
};
// Installs the OS handler: "" or the reason it cannot (per OS: crash_handler_win32.cpp,
// crash_handler_posix.cpp).
[[nodiscard]] std::string installBackend(const CrashBackendSetup& setup);
// Crashes as `mode` asks (crashIfRequested's modes); returns for any other value. Per OS.
void crashNow(std::string_view mode);
[[nodiscard]] bool isValidAppName(std::string_view app) noexcept;
[[nodiscard]] bool isValidProcessName(std::string_view process) noexcept;
// Writes `value` in `base` (10 or 16, lower-case) with at least `min_digits` digits to the front
// of `out`; returns the count, 0 when it does not fit. Async-signal-safe; formatCrashStem and the
// "crash signal" / "crash exception" lines use it.
[[nodiscard]] std::size_t appendNumber(std::span<char> out, std::uint64_t value, unsigned base,
                                       unsigned min_digits) noexcept;
// True for exactly "<process>-<8 digits>-<6 digits>-<1 to 10 digits>.dvvlog".
[[nodiscard]] bool isSnapshotName(std::string_view file_name, std::string_view process) noexcept;
// "<process>-<yyyymmdd-hhmmss>-<pid>" from local seconds since 1970 (days-from-civil arithmetic,
// no library call). Writes at most `out.size()` bytes and returns the count, 0 when it does not
// fit. Async-signal-safe (the POSIX handler uses it).
[[nodiscard]] std::size_t formatCrashStem(std::span<char> out, std::string_view process,
                                          std::int64_t local_seconds, std::uint64_t pid) noexcept;
// Logs and renames up to `limit` unreported snapshots of `process` in `folder`, oldest first.
void reportEarlierSnapshots(const std::filesystem::path& folder, std::string_view process,
                            std::size_t limit);
}  // namespace dvv::platform::detail
