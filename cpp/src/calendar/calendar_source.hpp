#pragma once
#include <ctime>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "calendar/ics_parser.hpp"

// Where the .ics text comes from. Both options are read-only and
// unauthenticated -- CallScribe never signs in to a Microsoft account.
namespace callscribe::calendar {

class CalendarError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

enum class SourceKind {
    Off,   // no calendar configured
    Url,   // published ICS feed, re-fetched on a timer
    File,  // a .ics exported by hand, re-read from disk
};

SourceKind source_kind_from_string(const std::string& s);
std::string source_kind_to_string(SourceKind kind);

// Downloads a published calendar. Follows redirects (Outlook's share links
// bounce at least once) and accepts webcal:// by rewriting it to https://.
// Throws CalendarError on a transport error or a non-2xx response.
std::string fetch_ics_url(const std::string& url, long timeout_seconds = 30);

// Reads a local .ics. Throws CalendarError if it can't be opened.
std::string read_ics_file(const std::filesystem::path& path);

// Last successfully fetched feed, kept so the agenda still renders when the
// machine is offline (and so a failed refresh degrades instead of blanking).
std::filesystem::path cache_file();
void write_cache(const std::string& ics_text);
std::string read_cache();  // "" when there is no cache yet
std::time_t cache_mtime();  // 0 when there is no cache

// Cheap sanity check before a download is written to the cache: an HTML
// error page from a revoked share link would otherwise silently replace a
// good feed with zero events.
bool looks_like_ics(const std::string& text);

struct LoadResult {
    std::vector<Event> events;
    std::string error;       // empty on success
    bool from_cache = false;  // the network/file read failed and this is the fallback
    std::time_t fetched_at = 0;
};

// Fetches (or reads) the configured source, refreshes the cache, and expands
// occurrences into [window_start, window_end). Falls back to the cache on
// failure, reporting the error alongside the stale events.
LoadResult load_calendar(SourceKind kind, const std::string& url, const std::string& file_path,
                          std::time_t window_start, std::time_t window_end);

}  // namespace callscribe::calendar
