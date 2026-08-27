#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace callscribe::util {

// Called periodically while a download runs, from the downloading thread.
// `total` is 0 when the server didn't send a content length, so callers must
// handle "bytes so far, size unknown" rather than assuming a percentage.
using ProgressCallback = std::function<void(int64_t downloaded, int64_t total)>;

// Downloads `url` to `dest`, creating parent directories as needed.
//
// Writes to a `.part` file and renames on success, so an interrupted or failed
// download can never leave a half-written model behind that later looks like a
// complete one. No timeout: these are model files on slow mirrors, and the
// caller runs this on a worker thread.
//
// Blocking. Throws std::runtime_error on any transport error or non-2xx reply.
void download_to_file(const std::string& url, const std::filesystem::path& dest,
                       const ProgressCallback& on_progress = nullptr);

}  // namespace callscribe::util
