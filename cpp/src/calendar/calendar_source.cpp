#include "calendar/calendar_source.hpp"

#include <curl/curl.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>

#include "config.hpp"

namespace callscribe::calendar {

namespace {

void ensure_curl_global_init() {
    static std::once_flag flag;
    std::call_once(flag, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buf = static_cast<std::string*>(userdata);
    buf->append(ptr, size * nmemb);
    return size * nmemb;
}

}  // namespace

SourceKind source_kind_from_string(const std::string& s) {
    if (s == "url") return SourceKind::Url;
    if (s == "file") return SourceKind::File;
    return SourceKind::Off;
}

std::string source_kind_to_string(SourceKind kind) {
    switch (kind) {
        case SourceKind::Url: return "url";
        case SourceKind::File: return "file";
        case SourceKind::Off: break;
    }
    return "off";
}

std::string fetch_ics_url(const std::string& url, long timeout_seconds) {
    ensure_curl_global_init();

    // Outlook and Apple hand out webcal:// links; it is plain HTTPS underneath.
    std::string effective = url;
    if (effective.rfind("webcal://", 0) == 0) effective = "https://" + effective.substr(9);

    CURL* curl = curl_easy_init();
    if (!curl) throw CalendarError("curl_easy_init failed");

    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, effective.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    // Published Outlook links redirect to a regional host before serving.
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "CallScribe/1.0");

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) throw CalendarError(std::string("Download failed: ") + curl_easy_strerror(res));
    if (http_code >= 400) {
        std::string hint;
        if (http_code == 401 || http_code == 403)
            hint = " -- the link may have been revoked, or publishing is disabled by your tenant";
        else if (http_code == 404)
            hint = " -- check the URL was copied in full";
        throw CalendarError("Server returned HTTP " + std::to_string(http_code) + hint);
    }
    return body;
}

std::string read_ics_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw CalendarError("Cannot open " + path.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::filesystem::path cache_file() { return data_dir() / "calendar-cache.ics"; }

void write_cache(const std::string& ics_text) {
    std::filesystem::create_directories(data_dir());
    // Written via a temp file so an interrupted write can't leave a truncated
    // cache behind as the only copy of the calendar.
    std::filesystem::path tmp = cache_file();
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out << ics_text;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, cache_file(), ec);
    if (ec) std::filesystem::remove(tmp, ec);
}

std::string read_cache() {
    std::ifstream in(cache_file(), std::ios::binary);
    if (!in) return "";
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::time_t cache_mtime() {
    std::error_code ec;
    auto t = std::filesystem::last_write_time(cache_file(), ec);
    if (ec) return 0;
    // No portable file_clock -> system_clock conversion before C++20's
    // clock_cast, which libstdc++ still lacks for file_clock; this offset
    // trick is accurate enough for an "updated N minutes ago" label.
    auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        t - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    return std::chrono::system_clock::to_time_t(sys);
}

bool looks_like_ics(const std::string& text) { return text.find("BEGIN:VCALENDAR") != std::string::npos; }

LoadResult load_calendar(SourceKind kind, const std::string& url, const std::string& file_path,
                          std::time_t window_start, std::time_t window_end) {
    LoadResult result;
    if (kind == SourceKind::Off) {
        result.error = "No calendar source configured.";
        return result;
    }

    std::string text;
    try {
        if (kind == SourceKind::Url) {
            if (url.empty()) throw CalendarError("No ICS URL set.");
            text = fetch_ics_url(url);
        } else {
            if (file_path.empty()) throw CalendarError("No .ics file path set.");
            text = read_ics_file(file_path);
        }
        if (!looks_like_ics(text)) {
            throw CalendarError(
                "That response wasn't an iCalendar file (no BEGIN:VCALENDAR) -- a sign-in page is the usual cause.");
        }
        write_cache(text);
        result.fetched_at = std::time(nullptr);
    } catch (const std::exception& e) {
        // Show the last good calendar rather than nothing, with the reason why
        // it is stale.
        result.error = e.what();
        text = read_cache();
        if (text.empty()) return result;
        result.from_cache = true;
        result.fetched_at = cache_mtime();
    }

    result.events = parse_ics(text, window_start, window_end);
    return result;
}

}  // namespace callscribe::calendar
