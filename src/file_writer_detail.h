#pragma once

// The private part of replaceFile: the temporary name, the name loop and the per-OS write and
// replace (the tests use them as a seam).

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>

#include "dvv/base/error.h"
#include "dvv/platform/file_writer.h"

namespace dvv::platform::detail {
[[nodiscard]] std::filesystem::path temporaryName(const std::filesystem::path& target,
                                                  std::uint64_t random);

struct WriteFailure {
    dvv::Error error;
    bool nameTaken = false;
};

// Writes `bytes` to the new file `temporary` and replaces `target`. On failure it removes
// `temporary`, except the kept lost-target case; `nameTaken`: `temporary` already existed, nothing
// was created or removed. `target_exists`: whether step 2 found the target. Per OS:
// file_writer_win32.cpp, file_writer_posix.cpp.
[[nodiscard]] std::expected<void, WriteFailure> writeAndReplace(
    const std::filesystem::path& target, const std::filesystem::path& temporary, bool target_exists,
    std::span<const std::byte> bytes, const ReplaceFileOptions& options);

// replaceFile after its file-name check, with the temporary names made from `randoms`: the first
// name, and the next one each time a name is taken (replaceFile passes 4 values from
// std::random_device). Returns the last error when every name is taken; Errc::Internal for no
// values. Messages as replaceFile's.
[[nodiscard]] Result<void> replaceFileWith(const std::filesystem::path& target,
                                           std::span<const std::byte> bytes,
                                           const ReplaceFileOptions& options,
                                           std::span<const std::uint64_t> randoms);
}  // namespace dvv::platform::detail
