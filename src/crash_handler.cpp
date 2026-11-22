#include "dvv/platform/crash_handler.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "crash_handler_detail.h"
#include "dvv/base/ascii.h"
#include "dvv/base/environment.h"
#include "dvv/base/log.h"
#include "dvv/base/paths.h"
#include "dvv/base/utf8.h"
#include "dvv/platform/file_reader.h"

namespace dvv::platform {

namespace detail {

namespace {

constexpr std::size_t MAX_NAME_BYTES = 63;
constexpr std::string_view SNAPSHOT_EXTENSION = ".dvvlog";
constexpr std::string_view REPORTED_EXTENSION = ".reported.dvvlog";

constexpr bool isDigit(char c) noexcept {
    return c >= '0' && c <= '9';
}

// Consumes `count_min` to `count_max` digits from the front of `text`; false when there are fewer.
constexpr bool skipDigits(std::string_view& text, std::size_t count_min,
                          std::size_t count_max) noexcept {
    std::size_t count = 0;
    while (count < text.size() && count < count_max && isDigit(text[count])) {
        ++count;
    }
    if (count < count_min) {
        return false;
    }
    text.remove_prefix(count);
    return true;
}

// Consumes `prefix` from the front of `text`; false when `text` does not start with it.
constexpr bool skipText(std::string_view& text, std::string_view prefix) noexcept {
    if (!text.starts_with(prefix)) {
        return false;
    }
    text.remove_prefix(prefix.size());
    return true;
}

// Logs and renames one snapshot file; nothing when another process renamed it first.
void reportSnapshot(const std::filesystem::path& file) {
    dvv::Logger& logger = dvv::Logger::instance();
    std::filesystem::path reported = file;
    reported.replace_extension();
    reported += std::filesystem::path(REPORTED_EXTENSION);
    std::error_code rename_error;
    std::filesystem::rename(file, reported, rename_error);
    if (rename_error) {
        // The file is gone: another process claimed it. Any other failure (a read-only folder)
        // leaves it unreported, so it says so.
        if (rename_error != std::errc::no_such_file_or_directory) {
            logger.write(dvv::LogLevel::Info,
                         std::format("an earlier run left a crash log that cannot be renamed "
                                     "({}): {}",
                                     dvv::pathDisplayText(file), rename_error.message()));
        }
        return;
    }
    const std::string shown = dvv::pathDisplayText(reported);
    const auto unreadable = [&](const dvv::Error& error) {
        logger.write(dvv::LogLevel::Info,
                     std::format("an earlier run left a crash log that cannot be read ({}): {}",
                                 shown, error.describe()));
    };
    auto reader = FileReader::open(reported);
    if (!reader) {
        unreadable(reader.error());
        return;
    }
    if (reader->size() > dvv::Logger::crashSnapshotCapacity()) {
        logger.write(dvv::LogLevel::Info,
                     std::format("an earlier run left a crash log too large to read ({})", shown));
        return;
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(reader->size()));
    const auto read = reader->readAt(0, bytes);
    if (!read) {
        unreadable(read.error());
        return;
    }
    bytes.resize(*read);
    logger.write(dvv::LogLevel::Info,
                 std::format("an earlier run crashed; its last log records follow ({})", shown));
    for (const std::string& line : dvv::decodeCrashSnapshot(bytes)) {
        logger.write(dvv::LogLevel::Info, line);
    }
}

}  // namespace

bool isValidAppName(std::string_view app) noexcept {
    return !app.empty() && app.size() <= MAX_NAME_BYTES && dvv::isValidUtf8(app);
}

bool isValidProcessName(std::string_view process) noexcept {
    // No leading '.': the crash files would be hidden files on macOS and Linux.
    return !process.empty() && process.size() <= MAX_NAME_BYTES && !process.starts_with('.') &&
           std::ranges::all_of(process, [](char c) {
               return isDigit(c) || dvv::isAsciiLetter(c) || c == '.' || c == '_' || c == '-';
           });
}

std::size_t appendNumber(std::span<char> out, std::uint64_t value, unsigned base,
                         unsigned min_digits) noexcept {
    if (base < 2 || base > 16) {
        return 0;
    }
    constexpr std::string_view DIGITS = "0123456789abcdef";
    std::array<char, 64> reversed{};  // 64 binary digits at most
    std::size_t digits = 0;
    do {
        reversed[digits] = DIGITS[static_cast<std::size_t>(value % base)];
        value /= base;
        ++digits;
    } while (value != 0);
    const std::size_t zeros = min_digits > digits ? min_digits - digits : 0;
    if (zeros > out.size() || digits > out.size() - zeros) {
        return 0;
    }
    std::ranges::fill(out.first(zeros), '0');
    for (std::size_t i = 0; i < digits; ++i) {
        out[zeros + i] = reversed[digits - 1 - i];
    }
    return zeros + digits;
}

bool appendText(std::span<char> out, std::size_t& length, std::string_view text) noexcept {
    if (length > out.size() || text.size() > out.size() - length) {
        return false;
    }
    std::ranges::copy(text, out.subspan(length).begin());
    length += text.size();
    return true;
}

bool appendNumberAt(std::span<char> out, std::size_t& length, std::uint64_t value, unsigned base,
                    unsigned min_digits) noexcept {
    if (length > out.size()) {
        return false;
    }
    const std::size_t count = appendNumber(out.subspan(length), value, base, min_digits);
    if (count == 0) {
        return false;
    }
    length += count;
    return true;
}

bool isSnapshotName(std::string_view file_name, std::string_view process) noexcept {
    std::string_view rest = file_name;
    return skipText(rest, process) && skipText(rest, "-") && skipDigits(rest, 8, 8) &&
           skipText(rest, "-") && skipDigits(rest, 6, 6) && skipText(rest, "-") &&
           skipDigits(rest, 1, 10) && rest == SNAPSHOT_EXTENSION;
}

std::size_t formatCrashStem(std::span<char> out, std::string_view process,
                            std::int64_t local_seconds, std::uint64_t pid) noexcept {
    constexpr std::int64_t DAY = 86400;
    // The times whose year has four digits, as isSnapshotName reads them. A clock outside them
    // gets the nearest one, so a crash snapshot is never lost over a clock value.
    constexpr std::int64_t FIRST_SECOND =
        std::chrono::sys_days{std::chrono::year{0} / 1 / 1}.time_since_epoch().count() * DAY;
    constexpr std::int64_t LAST_SECOND =
        (std::chrono::sys_days{std::chrono::year{9999} / 12 / 31}.time_since_epoch().count() *
         DAY) +
        DAY - 1;
    const std::int64_t seconds = std::clamp(local_seconds, FIRST_SECOND, LAST_SECOND);
    // Floor division: a time before 1970 belongs to the day before.
    std::int64_t days = seconds / DAY;
    std::int64_t second_of_day = seconds % DAY;
    if (second_of_day < 0) {
        second_of_day += DAY;
        --days;
    }
    const std::chrono::year_month_day date{
        std::chrono::sys_days{std::chrono::days{static_cast<std::chrono::days::rep>(days)}}};
    const auto time_of_day = static_cast<std::uint64_t>(second_of_day);
    std::size_t length = 0;
    const bool fits =
        appendText(out, length, process) && appendText(out, length, "-") &&
        appendNumberAt(out, length, static_cast<std::uint64_t>(static_cast<int>(date.year())), 10,
                       4) &&
        appendNumberAt(out, length, static_cast<unsigned>(date.month()), 10, 2) &&
        appendNumberAt(out, length, static_cast<unsigned>(date.day()), 10, 2) &&
        appendText(out, length, "-") && appendNumberAt(out, length, time_of_day / 3600, 10, 2) &&
        appendNumberAt(out, length, (time_of_day / 60) % 60, 10, 2) &&
        appendNumberAt(out, length, time_of_day % 60, 10, 2) && appendText(out, length, "-") &&
        appendNumberAt(out, length, pid, 10, 1);
    return fits ? length : 0;
}

std::string backendWarning(const BackendStatus& status) {
    if (!status.off.empty()) {
        return std::format("crash handler is off: {}", status.off);
    }
    if (!status.noDumps.empty()) {
        return std::format("crash dumps are off: {}", status.noDumps);
    }
    return {};
}

void reportEarlierSnapshots(const std::filesystem::path& folder, std::string_view process,
                            std::size_t limit) {
    std::error_code error;
    std::filesystem::directory_iterator entries(folder, error);
    if (error) {
        return;  // missing or unreadable: nothing to report
    }
    // Name and path of each snapshot; the names sort oldest first (their time part is
    // <yyyymmdd-hhmmss>).
    std::vector<std::pair<std::string, std::filesystem::path>> snapshots;
    for (; !error && entries != std::filesystem::directory_iterator(); entries.increment(error)) {
        const std::filesystem::directory_entry& entry = *entries;
        std::error_code status_error;
        if (!entry.is_regular_file(status_error) || status_error) {
            continue;
        }
        std::string name = dvv::pathText(entry.path().filename());
        if (isSnapshotName(name, process)) {
            snapshots.emplace_back(std::move(name), entry.path());
        }
    }
    std::ranges::sort(snapshots, {},
                      [](const auto& snapshot) -> const std::string& { return snapshot.first; });
    const std::size_t count = std::min(limit, snapshots.size());
    for (std::size_t i = 0; i < count; ++i) {
        reportSnapshot(snapshots[i].second);
    }
}

}  // namespace detail

void installCrashHandler(const CrashHandlerOptions& options) {
    static std::once_flag once;
    std::call_once(once, [&options] {
        dvv::Logger& logger = dvv::Logger::instance();
        const auto warn = [&logger](std::string_view reason) {
            logger.write(dvv::LogLevel::Warn, std::format("crash handler is off: {}", reason));
        };
        if (!detail::isValidAppName(options.app)) {
            warn("the app name is not 1 to 63 bytes of UTF-8");
            return;
        }
        if (!detail::isValidProcessName(options.process)) {
            warn(
                std::format("the process name '{}' is not 1 to 63 ASCII letters, digits, '.', "
                            "'_' or '-' that do not start with '.'",
                            options.process));
            return;
        }
        const auto base = dvv::paths::userDataDir(options.app);
        if (!base) {
            warn(base.error().describe());
            return;
        }
        const detail::CrashBackendOptions backend{.folder = *base / "crashes",
                                                  .app = options.app,
                                                  .process = options.process,
                                                  .logger = &logger};
        if (const std::string warning = detail::backendWarning(detail::installBackend(backend));
            !warning.empty()) {
            logger.write(dvv::LogLevel::Warn, warning);
        }
        detail::reportEarlierSnapshots(backend.folder, options.process, 8);
    });
}

void crashIfRequested(std::string_view variable) {
    const std::optional<std::string> value = dvv::getEnvironment(variable);
    detail::crashNow(value.value_or(std::string()));
}

}  // namespace dvv::platform
