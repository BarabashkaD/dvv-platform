#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <mutex>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

#include "dvv/base/error.h"
#include "dvv/base/utf8.h"
#include "dvv/platform/file_writer.h"
#include "file_writer_detail.h"
#if defined(_WIN32)
#include "file_writer_win32.h"
#else
#include "file_writer_posix.h"
#endif
#include "test_support.h"

namespace {

namespace fs = std::filesystem;
namespace detail = dvv::platform::detail;
using dvv::platform::FileDurability;
using dvv::platform::LostTarget;
using dvv::platform::replaceFile;

std::span<const std::byte> bytesOf(std::string_view text) {
    return std::as_bytes(std::span(text.data(), text.size()));
}

std::string readFile(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void writeBytes(const fs::path& path, std::string_view text) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    REQUIRE(stream.is_open());
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(stream.good());
}

// The start of every Errc::Io message of replaceFile for `target`.
std::string failurePrefix(const fs::path& target) {
    return "replacing '" + dvv::pathDisplayText(target) + "' failed: ";
}

// Whether `folder` holds a directory entry whose name ends in ".tmp".
bool hasTemporary(const fs::path& folder) {
    for (const fs::directory_entry& entry : fs::directory_iterator(folder)) {
        if (entry.path().filename().u8string().ends_with(u8".tmp")) {
            return true;
        }
    }
    return false;
}

#if !defined(_WIN32)
// Fakes for the ModeSetter seam: plain functions, so they count in globals.
std::atomic<int> mode_setter_calls{0};
std::atomic<mode_t> mode_before_setter{0};

// Records the temporary file's mode as open() created it, then sets the mode.
int recordingSetter(int file, mode_t mode) {
    ++mode_setter_calls;
    if (struct stat status{}; fstat(file, &status) == 0) {
        mode_before_setter = static_cast<mode_t>(status.st_mode & 0777);
    }
    return ::fchmod(file, mode);
}

// A file system without POSIX modes.
int refusingSetter(int /*file*/, mode_t /*mode*/) {
    ++mode_setter_calls;
    errno = EPERM;
    return -1;
}

int failingSetter(int /*file*/, mode_t /*mode*/) {
    ++mode_setter_calls;
    errno = EIO;
    return -1;
}

int throwingSetter(int /*file*/, mode_t /*mode*/) {
    ++mode_setter_calls;
    throw std::bad_alloc();
}

// A target at `mode`, holding "old".
fs::path targetWithMode(const fs::path& folder, fs::perms mode) {
    const fs::path target = folder / "a.json";
    writeBytes(target, "old");
    fs::permissions(target, mode, fs::perm_options::replace);
    return target;
}

// Sets the umask and restores the old one, also when an exception leaves the scope.
class UmaskScope {
public:
    explicit UmaskScope(mode_t mask) : old_mask_(::umask(mask)) {}
    UmaskScope(const UmaskScope&) = delete;
    UmaskScope& operator=(const UmaskScope&) = delete;
    UmaskScope(UmaskScope&&) = delete;
    UmaskScope& operator=(UmaskScope&&) = delete;
    ~UmaskScope() { ::umask(old_mask_); }

private:
    mode_t old_mask_;
};

// writeAndReplaceWith on an existing target under `mask`, with a fresh call count.
std::expected<void, detail::WriteFailure> replaceUnderMask(const fs::path& target, mode_t mask,
                                                           detail::ModeSetter set_mode) {
    mode_setter_calls = 0;
    mode_before_setter = 0;
    const UmaskScope umask_scope(mask);
    return detail::writeAndReplaceWith(target, detail::temporaryName(target, 1), true,
                                       bytesOf("new"), {}, set_mode);
}

constexpr fs::perms MODE_0600 = fs::perms::owner_read | fs::perms::owner_write;
constexpr fs::perms MODE_0640 = MODE_0600 | fs::perms::group_read;
#endif

}  // namespace

TEST_SUITE("file_writer") {
    TEST_CASE("replaceFile creates a missing target") {
        const dvv::platform::test::TemporaryDirectory dir("dvv-file-writer");
        const fs::path target = dir.path() / "a.json";
        const auto result = replaceFile(target, bytesOf("{\"a\": 1}\n"));
        CHECK(result.has_value());
        CHECK(readFile(target) == "{\"a\": 1}\n");
        CHECK_FALSE(hasTemporary(dir.path()));
    }

    TEST_CASE("replaceFile replaces an existing target") {
        const dvv::platform::test::TemporaryDirectory dir("dvv-file-writer");
        const fs::path target = dir.path() / "a.json";
        writeBytes(target, "old contents, longer than the new ones");
        const auto result = replaceFile(target, bytesOf("new"));
        CHECK(result.has_value());
        CHECK(readFile(target) == "new");
        CHECK_FALSE(hasTemporary(dir.path()));
    }

    TEST_CASE("replaceFile writes an empty file") {
        const dvv::platform::test::TemporaryDirectory dir("dvv-file-writer");
        const fs::path target = dir.path() / "a.json";
        writeBytes(target, "old");
        const auto result = replaceFile(target, {});
        CHECK(result.has_value());
        CHECK(fs::file_size(target) == 0);
        CHECK_FALSE(hasTemporary(dir.path()));
    }

#if !defined(_WIN32)
    TEST_CASE("replaceFile keeps the permission bits") {
        const dvv::platform::test::TemporaryDirectory dir("dvv-file-writer");
        const fs::path target = dir.path() / "a.json";
        writeBytes(target, "old");
        constexpr fs::perms MODE =
            fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read;  // 0640
        fs::permissions(target, MODE, fs::perm_options::replace);
        CHECK(fs::status(target).permissions() == MODE);
        const auto result = replaceFile(target, bytesOf("new"));
        CHECK(result.has_value());
        CHECK(fs::status(target).permissions() == MODE);
        CHECK(readFile(target) == "new");
    }

    TEST_CASE("a 0600 target stays 0600 and its temporary is never created wider") {
        const dvv::platform::test::TemporaryDirectory dir("dvv-file-writer");
        const fs::path target = targetWithMode(dir.path(), MODE_0600);
        CHECK(detail::temporaryMode(target, true) == 0600);
        CHECK(detail::temporaryMode(dir.path() / "new.json", false) == 0666);
        // Under umask 0 the temporary file has exactly the mode open() received. writeAndReplace
        // reads it with fstat and calls the setter only when it differs from 0600: no call means
        // the temporary file was created 0600.
        const auto result = replaceUnderMask(target, 0, recordingSetter);
        CHECK(result.has_value());
        CHECK(mode_setter_calls.load() == 0);
        CHECK(mode_before_setter.load() == 0);  // the created mode, had the setter run
        CHECK(fs::status(target).permissions() == MODE_0600);
        CHECK(readFile(target) == "new");
    }

    TEST_CASE("the mode is set only when the umask removed bits") {
        const dvv::platform::test::TemporaryDirectory dir("dvv-file-writer");
        const fs::path target = targetWithMode(dir.path(), MODE_0640);
        const auto unmasked = replaceUnderMask(target, 0, recordingSetter);
        CHECK(unmasked.has_value());
        CHECK(mode_setter_calls.load() == 0);
        const auto masked = replaceUnderMask(target, 077, recordingSetter);
        CHECK(masked.has_value());
        CHECK(mode_setter_calls.load() == 1);
        CHECK(mode_before_setter.load() == 0600);  // 0640 less the umask, never wider
        CHECK(fs::status(target).permissions() == MODE_0640);
    }

    TEST_CASE("a file system without modes does not fail the replace") {
        const dvv::platform::test::TemporaryDirectory dir("dvv-file-writer");
        const fs::path target = targetWithMode(dir.path(), MODE_0640);
        const auto result = replaceUnderMask(target, 077, refusingSetter);
        CHECK(result.has_value());
        CHECK(mode_setter_calls.load() == 1);
        CHECK(readFile(target) == "new");
        CHECK_FALSE(hasTemporary(dir.path()));
    }

    TEST_CASE("another mode error fails the replace and removes the temporary") {
        const dvv::platform::test::TemporaryDirectory dir("dvv-file-writer");
        const fs::path target = targetWithMode(dir.path(), MODE_0640);
        const auto result = replaceUnderMask(target, 077, failingSetter);
        CHECK_FALSE(result.has_value());
        if (!result) {
            CHECK(result.error().error.code == dvv::Errc::Io);
            CHECK(result.error().error.message.starts_with("cannot set the permissions: "));
            CHECK_FALSE(result.error().nameTaken);
        }
        CHECK(readFile(target) == "old");
        CHECK_FALSE(hasTemporary(dir.path()));
    }

    TEST_CASE("an exception after the create removes the temporary") {
        const dvv::platform::test::TemporaryDirectory dir("dvv-file-writer");
        const fs::path target = targetWithMode(dir.path(), MODE_0640);
        const auto replace = [&target] {
            const auto result = replaceUnderMask(target, 077, throwingSetter);
            CHECK_FALSE(result.has_value());  // not reached: the setter throws
        };
        CHECK_THROWS_AS(replace(), std::bad_alloc);
        CHECK(mode_setter_calls.load() == 1);
        CHECK(readFile(target) == "old");
        CHECK_FALSE(hasTemporary(dir.path()));
    }

    TEST_CASE("a 0640 target under umask 077 stays 0640") {
        const dvv::platform::test::TemporaryDirectory dir("dvv-file-writer");
        const fs::path target = dir.path() / "a.json";
        writeBytes(target, "old");
        constexpr fs::perms MODE =
            fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read;  // 0640
        fs::permissions(target, MODE, fs::perm_options::replace);
        const mode_t old_mask = ::umask(077);
        const auto result = replaceFile(target, bytesOf("new"));
        ::umask(old_mask);
        CHECK(result.has_value());
        CHECK(fs::status(target).permissions() == MODE);
    }

    TEST_CASE("a new target gets the umask's default") {
        const dvv::platform::test::TemporaryDirectory dir("dvv-file-writer");
        const fs::path reference = dir.path() / "reference.json";
        writeBytes(reference, "reference");
        const fs::path target = dir.path() / "a.json";
        const auto result = replaceFile(target, bytesOf("new"));
        CHECK(result.has_value());
        CHECK(fs::status(target).permissions() == fs::status(reference).permissions());
    }

    TEST_CASE("a new target under umask 077 is 0600") {
        const dvv::platform::test::TemporaryDirectory dir("dvv-file-writer");
        const fs::path target = dir.path() / "a.json";
        const mode_t old_mask = ::umask(077);
        const auto result = replaceFile(target, bytesOf("new"));
        ::umask(old_mask);
        CHECK(result.has_value());
        CHECK(fs::status(target).permissions() == (fs::perms::owner_read | fs::perms::owner_write));
    }
#endif

    TEST_CASE("replaceFile handles a non-ASCII target name") {
        const dvv::platform::test::TemporaryDirectory dir("dvv-file-writer");
        const fs::path target = dir.path() / fs::path(u8"ü-ö.json");
        const auto created = replaceFile(target, bytesOf("first"));
        CHECK(created.has_value());
        CHECK(readFile(target) == "first");
        const auto replaced = replaceFile(target, bytesOf("second"));
        CHECK(replaced.has_value());
        CHECK(readFile(target) == "second");
        CHECK_FALSE(hasTemporary(dir.path()));
    }

    TEST_CASE("replaceFile fails in a missing folder") {
        const dvv::platform::test::TemporaryDirectory dir("dvv-file-writer");
        const fs::path target = dir.path() / "missing" / "a.json";
        const auto result = replaceFile(target, bytesOf("x"));
        CHECK_FALSE(result.has_value());
        if (!result) {
            CHECK(result.error().code == dvv::Errc::Io);
            CHECK(result.error().message.starts_with(failurePrefix(target) +
                                                     "cannot create a temporary file: "));
        }
        CHECK_FALSE(fs::exists(dir.path() / "missing"));
    }

    TEST_CASE("replaceFile refuses a folder and a link") {
        const dvv::platform::test::TemporaryDirectory dir("dvv-file-writer");
        const fs::path folder = dir.path() / "folder";
        fs::create_directory(folder);
        const auto refused_folder = replaceFile(folder, bytesOf("x"));
        CHECK_FALSE(refused_folder.has_value());
        if (!refused_folder) {
            CHECK(refused_folder.error().code == dvv::Errc::Io);
            CHECK(refused_folder.error().message == failurePrefix(folder) + "not a regular file");
        }
        CHECK(fs::is_directory(folder));
#if !defined(_WIN32)
        const fs::path file = dir.path() / "file.json";
        writeBytes(file, "file");
        const fs::path link = dir.path() / "link.json";
        fs::create_symlink(file, link);
        const auto refused_link = replaceFile(link, bytesOf("x"));
        CHECK_FALSE(refused_link.has_value());
        if (!refused_link) {
            CHECK(refused_link.error().code == dvv::Errc::Io);
            CHECK(refused_link.error().message == failurePrefix(link) + "not a regular file");
        }
        CHECK(fs::is_symlink(fs::symlink_status(link)));
        CHECK(readFile(file) == "file");
#endif
        CHECK_FALSE(hasTemporary(dir.path()));
    }

#if defined(_WIN32)
    TEST_CASE("the retryable replace errors") {
        for (const DWORD code :
             std::initializer_list<DWORD>{ERROR_SHARING_VIOLATION, ERROR_LOCK_VIOLATION,
                                          ERROR_ACCESS_DENIED, ERROR_UNABLE_TO_REMOVE_REPLACED}) {
            CHECK(detail::isRetryableReplaceError(code, false));
            CHECK(detail::isRetryableReplaceError(code, true));
        }
        for (const DWORD code :
             std::initializer_list<DWORD>{ERROR_ALREADY_EXISTS, ERROR_FILE_EXISTS}) {
            CHECK_FALSE(detail::isRetryableReplaceError(code, false));
            CHECK(detail::isRetryableReplaceError(code, true));  // another writer created it
        }
        // ReplaceFileW while another writer's replace has the target away from its name; both
        // files keep their names. A move to a new target that fails with it does not retry.
        CHECK(detail::isRetryableReplaceError(ERROR_FILE_NOT_FOUND, false));
        CHECK_FALSE(detail::isRetryableReplaceError(ERROR_FILE_NOT_FOUND, true));
        for (const DWORD code : std::initializer_list<DWORD>{
                 ERROR_UNABLE_TO_MOVE_REPLACEMENT, ERROR_UNABLE_TO_MOVE_REPLACEMENT_2,
                 ERROR_PATH_NOT_FOUND, ERROR_DISK_FULL}) {
            CHECK_FALSE(detail::isRetryableReplaceError(code, false));
            CHECK_FALSE(detail::isRetryableReplaceError(code, true));
        }
    }

    TEST_CASE("a held target fails without retries") {
        const dvv::platform::test::TemporaryDirectory dir("dvv-file-writer");
        const fs::path target = dir.path() / "a.json";
        writeBytes(target, "old");
        const HANDLE held =
            CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE(held != INVALID_HANDLE_VALUE);
        const auto result = replaceFile(target, bytesOf("new"), {.retries = 0});
        CloseHandle(held);
        CHECK_FALSE(result.has_value());
        if (!result) {
            CHECK(result.error().code == dvv::Errc::Io);
            CHECK(result.error().message.starts_with(failurePrefix(target) +
                                                     "cannot replace the file: "));
        }
        CHECK(readFile(target) == "old");
        CHECK_FALSE(hasTemporary(dir.path()));
    }

    TEST_CASE("a held target succeeds with retries once released") {
        const dvv::platform::test::TemporaryDirectory dir("dvv-file-writer");
        const fs::path target = dir.path() / "a.json";
        writeBytes(target, "old");
        const HANDLE held =
            CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE(held != INVALID_HANDLE_VALUE);
        std::jthread release([held] {
            std::this_thread::sleep_for(std::chrono::milliseconds{80});
            CloseHandle(held);
        });
        const auto result = replaceFile(
            target, bytesOf("new"), {.retries = 5, .retryDelay = std::chrono::milliseconds{50}});
        release.join();
        CHECK(result.has_value());
        CHECK(readFile(target) == "new");
        CHECK_FALSE(hasTemporary(dir.path()));
    }
#endif

    TEST_CASE("concurrent replaces leave one complete file") {
        const dvv::platform::test::TemporaryDirectory dir("dvv-file-writer");
        const fs::path target = dir.path() / "a.json";
        const std::string first(4096, 'a');
        const std::string second(8192, 'b');
        std::atomic<int> failures{0};
        std::mutex message_mutex;
        std::string first_message;
        const auto replace_many = [&](const std::string& payload) {
            for (int i = 0; i < 25; ++i) {
                const auto result =
                    replaceFile(target, bytesOf(payload),
                                {.retries = 5, .lostTarget = LostTarget::RemoveTemporary});
                if (!result) {
                    failures.fetch_add(1);
                    const std::scoped_lock lock(message_mutex);
                    if (first_message.empty()) {
                        first_message = result.error().message;
                    }
                }
            }
        };
        {
            std::jthread one([&] { replace_many(first); });
            std::jthread two([&] { replace_many(second); });
        }
        CHECK_MESSAGE(failures.load() == 0, first_message);
        const std::string contents = readFile(target);
        CHECK((contents == first || contents == second));
        CHECK_FALSE(hasTemporary(dir.path()));
    }

    TEST_CASE("the temporary name") {
        CHECK(detail::temporaryName("a/b.json", 0x1f) == fs::path("a/b.json.000000000000001f.tmp"));
    }

    TEST_CASE("Unflushed writes the same bytes") {
        const dvv::platform::test::TemporaryDirectory dir("dvv-file-writer");
        const fs::path target = dir.path() / "a.json";
        writeBytes(target, "old");
        const auto result =
            replaceFile(target, bytesOf("unflushed"), {.durability = FileDurability::Unflushed});
        CHECK(result.has_value());
        CHECK(readFile(target) == "unflushed");
        CHECK_FALSE(hasTemporary(dir.path()));
    }

    TEST_CASE("a taken temporary name is reported and left alone") {
        const dvv::platform::test::TemporaryDirectory dir("dvv-file-writer");
        const fs::path target = dir.path() / "a.json";
        const fs::path taken = detail::temporaryName(target, 7);
        writeBytes(taken, "other writer");  // the helper writes a file with std::ofstream
        const auto result = detail::writeAndReplace(target, taken, false, bytesOf("x"), {});
        CHECK_FALSE(result.has_value());
        if (!result) {
            CHECK(result.error().nameTaken);
        }
        CHECK(readFile(taken) == "other writer");
        CHECK_FALSE(fs::exists(target));
    }

    TEST_CASE("a taken temporary name moves on to the next one") {
        const dvv::platform::test::TemporaryDirectory dir("dvv-file-writer");
        const fs::path target = dir.path() / "a.json";
        const fs::path taken = detail::temporaryName(target, 1);
        writeBytes(taken, "other writer");
        const std::array<std::uint64_t, 4> randoms = {1, 2, 3, 4};
        const auto result = detail::replaceFileWith(target, bytesOf("new"), {}, randoms);
        CHECK(result.has_value());
        CHECK(readFile(target) == "new");
        CHECK(readFile(taken) == "other writer");  // another writer's file is never removed
        for (const std::uint64_t random : std::initializer_list<std::uint64_t>{2, 3, 4}) {
            CHECK_FALSE(fs::exists(detail::temporaryName(target, random)));
        }
    }

    TEST_CASE("four taken temporary names fail and leave every file alone") {
        const dvv::platform::test::TemporaryDirectory dir("dvv-file-writer");
        const fs::path target = dir.path() / "a.json";
        writeBytes(target, "old");
        const std::array<std::uint64_t, 4> randoms = {1, 2, 3, 4};
        for (const std::uint64_t random : randoms) {
            writeBytes(detail::temporaryName(target, random), "other writer");
        }
        const auto result = detail::replaceFileWith(target, bytesOf("new"), {}, randoms);
        CHECK_FALSE(result.has_value());
        if (!result) {
            CHECK(result.error().code == dvv::Errc::Io);
            CHECK(result.error().message.starts_with(failurePrefix(target) +
                                                     "cannot create a temporary file: "));
        }
        CHECK(readFile(target) == "old");
        for (const std::uint64_t random : randoms) {
            CHECK(readFile(detail::temporaryName(target, random)) == "other writer");
        }
        const auto none = detail::replaceFileWith(target, bytesOf("new"), {}, {});
        CHECK_FALSE(none.has_value());
        if (!none) {
            CHECK(none.error().code == dvv::Errc::Internal);
        }
    }

    TEST_CASE("the edges of the target path") {
        const dvv::platform::test::TemporaryDirectory dir("dvv-file-writer");
        CHECK(detail::temporaryName("b.json", 1) == fs::path("b.json.0000000000000001.tmp"));
        for (const fs::path& target : {fs::path(), dir.path() / "sub" / ""}) {
            const auto result = dvv::platform::replaceFile(target, bytesOf("x"));
            CHECK_FALSE(result.has_value());
            if (!result) {
                CHECK(result.error().code == dvv::Errc::Internal);
                CHECK(result.error().message.ends_with("names no file"));
            }
        }
        CHECK(fs::is_empty(dir.path()));
    }
}
