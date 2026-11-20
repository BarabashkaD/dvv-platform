#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include "dvv/base/log.h"

namespace dvv::platform::detail {
struct CrashBackendOptions {
    std::filesystem::path folder;  // <userDataDir(app)>/crashes; its parents are made at a crash
    std::string app;
    std::string process;
    const dvv::Logger* logger = nullptr;
};
// What installBackend did. `off` non-empty: the handler is off, for that reason. Else `noDumps`
// non-empty: the handler is on but writes no minidump, for that reason (Windows: dbghelp.dll or
// its MiniDumpWriteDump missing, or no dump thread); a crash still writes the cause line and the
// snapshot.
struct BackendStatus {
    std::string off{};
    std::string noDumps{};
};
// Installs the OS handler (per OS: crash_handler_win32.cpp, crash_handler_posix.cpp).
[[nodiscard]] BackendStatus installBackend(const CrashBackendOptions& options);
// The one Warn line for `status`: "crash handler is off: <off>", else "crash dumps are off:
// <noDumps>", else "" (installed in full).
[[nodiscard]] std::string backendWarning(const BackendStatus& status);
// Crashes as `mode` asks (crashIfRequested's modes); returns for any other value. Per OS.
void crashNow(std::string_view mode);
[[nodiscard]] bool isValidAppName(std::string_view app) noexcept;
[[nodiscard]] bool isValidProcessName(std::string_view process) noexcept;
// Writes `value` in `base` (10 or 16, lower-case) with at least `min_digits` digits to the front
// of `out`; returns the count, 0 when it does not fit. Async-signal-safe; formatCrashStem and the
// "crash signal" / "crash exception" lines use it.
[[nodiscard]] std::size_t appendNumber(std::span<char> out, std::uint64_t value, unsigned base,
                                       unsigned min_digits) noexcept;
// Copies `text` to `out` at `length` and advances `length`; false (nothing copied) when it does
// not fit. Async-signal-safe.
[[nodiscard]] bool appendText(std::span<char> out, std::size_t& length,
                              std::string_view text) noexcept;
// appendNumber at `length`, advancing `length`; false (nothing written) when it does not fit.
// Async-signal-safe.
[[nodiscard]] bool appendNumberAt(std::span<char> out, std::size_t& length, std::uint64_t value,
                                  unsigned base, unsigned min_digits) noexcept;
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
