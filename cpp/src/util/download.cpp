#include "util/download.hpp"

#include <curl/curl.h>

#include <cstdio>
#include <stdexcept>

namespace callscribe::util {

namespace {

// curl hands progress to a C callback; this carries the std::function across.
int xfer_callback(void* userdata, curl_off_t total, curl_off_t downloaded, curl_off_t, curl_off_t) {
    const auto* on_progress = static_cast<const ProgressCallback*>(userdata);
    if (on_progress && *on_progress)
        (*on_progress)(static_cast<int64_t>(downloaded), static_cast<int64_t>(total));
    return 0;  // non-zero would abort the transfer
}

}  // namespace

void download_to_file(const std::string& url, const std::filesystem::path& dest,
                       const ProgressCallback& on_progress) {
    if (dest.has_parent_path()) std::filesystem::create_directories(dest.parent_path());

    std::filesystem::path tmp_path = dest;
    tmp_path += ".part";

    FILE* fp = std::fopen(tmp_path.string().c_str(), "wb");
    if (!fp) throw std::runtime_error("Failed to open " + tmp_path.string() + " for writing");

    CURL* curl = curl_easy_init();
    if (!curl) {
        std::fclose(fp);
        throw std::runtime_error("curl_easy_init failed");
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
    if (on_progress) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xfer_callback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &on_progress);
    }
    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    std::fclose(fp);

    if (res != CURLE_OK || http_code >= 400) {
        std::filesystem::remove(tmp_path);
        std::string reason = res != CURLE_OK ? curl_easy_strerror(res) : ("HTTP " + std::to_string(http_code));
        throw std::runtime_error("Failed to download " + url + ": " + reason);
    }
    std::filesystem::rename(tmp_path, dest);
}

}  // namespace callscribe::util
