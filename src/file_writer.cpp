#include "dvv/platform/file_writer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <random>
#include <span>
#include <system_error>
#include <utility>

#include "dvv/base/error.h"
#include "dvv/base/utf8.h"
#include "file_writer_detail.h"

namespace dvv::platform {

namespace {

// The first temporary name and up to 3 more when a name is taken.
constexpr std::size_t NAME_ATTEMPTS = 4;

// One 64-bit value from std::random_device per temporary name. The device may throw
// std::system_error, when it is made or called; that becomes an error here.
Result<std::array<std::uint64_t, NAME_ATTEMPTS>> nameRandoms() {
    try {
        std::random_device device;
        std::array<std::uint64_t, NAME_ATTEMPTS> randoms{};
        for (std::uint64_t& random : randoms) {
            random = (std::uint64_t{device()} << 32U) | std::uint64_t{device()};
        }
        return randoms;
    } catch (const std::system_error& error) {
        return makeError(Errc::Io, std::format("cannot create a temporary file: {}", error.what()));
    }
}

}  // namespace

namespace detail {

std::filesystem::path temporaryName(const std::filesystem::path& target, std::uint64_t random) {
    std::filesystem::path name = target;  // the folder and the file name
    name += std::format(".{:016x}.tmp", random);
    return name;
}

}  // namespace detail

Result<void> replaceFile(const std::filesystem::path& target, std::span<const std::byte> bytes,
                         const ReplaceFileOptions& options) {
    if (!target.has_filename()) {
        return makeError(Errc::Internal,
                         std::format("replaceFile: '{}' names no file", pathDisplayText(target)));
    }

    // The type first: a missing target is not_found with `error` also set (libstdc++, MSVC).
    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::symlink_status(target, error);
    const std::filesystem::file_type type = status.type();
    if (type == std::filesystem::file_type::none) {
        return makeError(Errc::Io,
                         std::format("cannot read the file's status: {}", error.message()));
    }
    const bool target_exists = type != std::filesystem::file_type::not_found;
    if (target_exists && type != std::filesystem::file_type::regular) {
        return makeError(Errc::Io,
                         std::format("'{}' is not a regular file", pathDisplayText(target)));
    }

    DVV_TRY(const auto randoms, nameRandoms());
    std::expected<void, detail::WriteFailure> written;
    for (const std::uint64_t random : randoms) {
        written = detail::writeAndReplace(target, detail::temporaryName(target, random),
                                          target_exists, bytes, options);
        if (written || !written.error().nameTaken) {
            break;
        }
    }
    if (!written) {
        return std::unexpected(std::move(written.error().error));
    }
    return {};
}

}  // namespace dvv::platform
