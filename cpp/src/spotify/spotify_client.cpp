#include "spotify/spotify_client.hpp"

#include <curl/curl.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <nlohmann/json.hpp>
#include <stdexcept>
#include <utility>
#include <vector>

#include "spotify/callback_listener.hpp"
#include "spotify/keyring.hpp"
#include "spotify/pkce.hpp"
#include "util/clipboard.hpp"

namespace callscribe::spotify {

namespace {

constexpr const char* kTokenUrl = "https://accounts.spotify.com/api/token";
constexpr const char* kApiBase = "https://api.spotify.com/v1";
// Long enough to log in and click through a consent screen, short enough that
// a forgotten login doesn't hold the port (and the worker) forever.
constexpr auto kLoginTimeout = std::chrono::minutes(5);

void ensure_curl_global_init() {
    static std::once_flag flag;
    std::call_once(flag, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
    return size * nmemb;
}

struct Response {
    long status = 0;
    std::string body;
};

Response http_request(const std::string& method, const std::string& url, const std::vector<std::string>& headers,
                      const std::string& body) {
    ensure_curl_global_init();
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("curl_easy_init failed");

    Response out;
    curl_slist* header_list = nullptr;
    for (const auto& h : headers) header_list = curl_slist_append(header_list, h.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    if (method != "GET") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
        // Set even when empty: Spotify answers a PUT with no Content-Length
        // with 411 Length Required.
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out.body);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out.status);
    curl_slist_free_all(header_list);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) throw std::runtime_error(std::string("Can't reach Spotify: ") + curl_easy_strerror(res));
    return out;
}

Response post_form(const std::string& url, const std::vector<std::pair<std::string, std::string>>& fields) {
    std::string body;
    for (const auto& [key, value] : fields) {
        if (!body.empty()) body += '&';
        body += url_encode(key) + '=' + url_encode(value);
    }
    return http_request("POST", url, {"Content-Type: application/x-www-form-urlencoded"}, body);
}

nlohmann::json parse_or_empty(const std::string& body) {
    auto j = nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
    return j.is_discarded() ? nlohmann::json::object() : j;
}

// Web API errors are {"error": {"status": 404, "message": "...", "reason": "..."}};
// the accounts service uses {"error": "invalid_grant", "error_description": "..."}.
std::string error_message(const std::string& body, long status) {
    auto j = parse_or_empty(body);
    if (j.contains("error")) {
        const auto& e = j["error"];
        if (e.is_object() && e.contains("message") && e["message"].is_string()) return e["message"].get<std::string>();
        if (j.contains("error_description") && j["error_description"].is_string())
            return j["error_description"].get<std::string>();
        if (e.is_string()) return e.get<std::string>();
    }
    return "HTTP " + std::to_string(status);
}

bool is_success(long status) { return status >= 200 && status < 300; }

// xdg-open, with every stdio stream pointed at /dev/null and its own session,
// so neither it nor a browser it starts can write over the TUI or read its
// keystrokes. Returns false if it is missing or reports failure; one that is
// still running after a few seconds (a browser launched in the foreground) is
// taken as success and reaped in the background.
bool open_in_browser(const std::string& url) {
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execlp("xdg-open", "xdg-open", url.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    for (int i = 0; i < 30; ++i) {
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid) return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::thread([pid] {
        int status = 0;
        waitpid(pid, &status, 0);
    }).detach();
    return true;
}

}  // namespace

int NowPlaying::progress_now_ms(std::chrono::steady_clock::time_point now) const {
    if (!playing) return progress_ms;
    auto since = std::chrono::duration_cast<std::chrono::milliseconds>(now - fetched_at).count();
    return static_cast<int>(std::min<long long>(duration_ms, progress_ms + since));
}

SpotifyClient::SpotifyClient() {
    worker_ = std::thread([this] {
        while (auto job = jobs_.pop()) {
            // Anything still queued at shutdown is dropped rather than run:
            // each could be a network round trip, and nobody is waiting on it.
            if (stopping_.load()) continue;
            try {
                (*job)();
            } catch (const std::exception& e) {
                set_status(e.what());
            }
        }
    });
}

SpotifyClient::~SpotifyClient() {
    stopping_.store(true);
    cancel_login_.store(true);
    jobs_.close();
    if (worker_.joinable()) worker_.join();
}

void SpotifyClient::configure(const std::string& client_id, int redirect_port) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (client_id != client_id_) {
        connected_.store(false);
        status_.clear();
        manual_login_url_.clear();
    }
    client_id_ = client_id;
    redirect_port_ = redirect_port;
}

bool SpotifyClient::configured() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !client_id_.empty();
}

std::string SpotifyClient::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

std::string SpotifyClient::manual_login_url() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return manual_login_url_;
}

void SpotifyClient::set_status(std::string status) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = std::move(status);
}

void SpotifyClient::enqueue(std::function<void()> job) {
    if (!configured()) return;
    jobs_.push(std::move(job));
}

void SpotifyClient::check() {
    enqueue([this] {
        if (ensure_refresh_token())
            set_status("Connected.");
        else if (status().empty())
            set_status("Not connected yet.");
    });
}

void SpotifyClient::connect() {
    // Claimed here rather than on the worker, so a double-click can't queue a
    // second login behind the first.
    if (!configured() || logging_in_.exchange(true)) return;
    cancel_login_.store(false);
    enqueue([this] { do_login(); });
}

void SpotifyClient::disconnect() {
    cancel_login_.store(true);
    enqueue([this] {
        std::string client_id;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            client_id = client_id_;
        }
        std::string error;
        clear_refresh_token(client_id, error);
        forget_session(error.empty() ? "Disconnected." : "Disconnected, but " + error);
    });
}

void SpotifyClient::play_playlist(const std::string& playlist) {
    enqueue([this, playlist] { do_play(playlist); });
}

void SpotifyClient::pause() {
    enqueue([this] { do_pause(); });
}

void SpotifyClient::toggle_playback(const std::string& fallback_playlist) {
    if (!connected()) return;
    enqueue([this, fallback_playlist] { do_toggle(fallback_playlist); });
}

void SpotifyClient::next_track() {
    if (!connected()) return;
    enqueue([this] {
        HttpResponse r = api("POST", "/me/player/next");
        if (!is_success(r.status) && r.status != 401)
            set_status("Couldn't skip: " + error_message(r.body, r.status));
        fetch_now_playing_after_command();
    });
}

void SpotifyClient::previous_track() {
    if (!connected()) return;
    enqueue([this] {
        HttpResponse r = api("POST", "/me/player/previous");
        if (!is_success(r.status) && r.status != 401)
            set_status("Couldn't go back: " + error_message(r.body, r.status));
        fetch_now_playing_after_command();
    });
}

NowPlaying SpotifyClient::now_playing() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return now_playing_;
}

void SpotifyClient::refresh_now_playing(std::chrono::milliseconds min_interval) {
    if (!connected()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now = std::chrono::steady_clock::now();
        if (fetch_queued_ || now - last_fetch_request_ < min_interval) return;
        fetch_queued_ = true;
        last_fetch_request_ = now;
    }
    enqueue([this] { fetch_now_playing(); });
}

void SpotifyClient::fetch_now_playing_after_command() {
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    fetch_now_playing();
}

void SpotifyClient::fetch_now_playing() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        fetch_queued_ = false;
    }
    if (!ensure_refresh_token()) return;
    HttpResponse r = api("GET", "/me/player");

    NowPlaying np;
    np.fetched_at = std::chrono::steady_clock::now();
    // 204: no device has anything loaded.
    if (r.status == 200) {
        auto j = parse_or_empty(r.body);
        np.active = true;
        np.playing = j.value("is_playing", false);
        np.progress_ms = j.value("progress_ms", 0);
        if (j.contains("device") && j["device"].is_object()) np.device = j["device"].value("name", "");
        // item is null during ads and for anything Spotify won't describe.
        if (j.contains("item") && j["item"].is_object()) {
            const auto& item = j["item"];
            np.track = item.value("name", "");
            np.duration_ms = item.value("duration_ms", 0);
            if (item.contains("artists") && item["artists"].is_array()) {
                for (const auto& a : item["artists"]) {
                    if (!np.artists.empty()) np.artists += ", ";
                    np.artists += a.value("name", "");
                }
            } else if (item.contains("show") && item["show"].is_object()) {
                np.artists = item["show"].value("name", "");  // podcast episode
            }
        }
    } else if (r.status != 204) {
        return;  // keep showing the last known state rather than blanking it
    }
    std::lock_guard<std::mutex> lock(mutex_);
    now_playing_ = std::move(np);
}

void SpotifyClient::forget_session(const std::string& reason) {
    refresh_token_.clear();
    access_token_.clear();
    connected_.store(false);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        now_playing_ = NowPlaying{};
    }
    set_status(reason);
}

bool SpotifyClient::ensure_refresh_token() {
    std::string client_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        client_id = client_id_;
    }
    if (client_id != token_client_id_) {
        token_client_id_ = client_id;
        keyring_checked_ = false;
        refresh_token_.clear();
        access_token_.clear();
    }
    if (!refresh_token_.empty()) return true;
    if (keyring_checked_) return false;

    keyring_checked_ = true;
    std::string error;
    auto token = lookup_refresh_token(client_id, error);
    if (!error.empty()) set_status(error);
    if (!token) return false;
    refresh_token_ = *token;
    connected_.store(true);
    return true;
}

bool SpotifyClient::refresh_access_token() {
    if (!ensure_refresh_token()) return false;
    Response r = post_form(kTokenUrl, {{"grant_type", "refresh_token"},
                                       {"refresh_token", refresh_token_},
                                       {"client_id", token_client_id_}});
    auto j = parse_or_empty(r.body);
    if (!is_success(r.status) || !j.contains("access_token")) {
        // invalid_grant: revoked from the Spotify account page, or expired.
        // Retrying can't fix that, so drop it and ask for a fresh login.
        if (j.value("error", "") == "invalid_grant") {
            std::string ignored;
            clear_refresh_token(token_client_id_, ignored);
            forget_session("Spotify access was revoked -- Connect again in Settings.");
        } else {
            set_status("Couldn't refresh the Spotify session: " + error_message(r.body, r.status));
        }
        return false;
    }
    access_token_ = j["access_token"].get<std::string>();
    access_expires_ = std::chrono::steady_clock::now() + std::chrono::seconds(j.value("expires_in", 3600) - 60);
    // Spotify may rotate the refresh token; the old one stops working once a
    // new one has been issued, so it has to be saved straight away.
    if (j.contains("refresh_token") && j["refresh_token"].is_string() &&
        j["refresh_token"].get<std::string>() != refresh_token_) {
        refresh_token_ = j["refresh_token"].get<std::string>();
        std::string error;
        if (!store_refresh_token(token_client_id_, refresh_token_, error)) set_status(error);
    }
    return true;
}

SpotifyClient::HttpResponse SpotifyClient::api(const std::string& method, const std::string& path,
                                               const std::string& body) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (access_token_.empty() || std::chrono::steady_clock::now() >= access_expires_) {
            if (!refresh_access_token()) return {401, ""};
        }
        std::vector<std::string> headers = {"Authorization: Bearer " + access_token_};
        if (!body.empty()) headers.push_back("Content-Type: application/json");
        Response r = http_request(method, std::string(kApiBase) + path, headers, body);
        if (r.status != 401) return {r.status, r.body};
        access_token_.clear();
    }
    return {401, ""};
}

void SpotifyClient::do_login() {
    std::string client_id;
    int port = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        client_id = client_id_;
        port = redirect_port_;
        manual_login_url_.clear();
    }
    struct Done {
        std::atomic<bool>& flag;
        ~Done() { flag.store(false); }
    } done{logging_in_};

    const std::string verifier = random_token(64);
    const std::string state = random_token(24);
    CallbackListener listener(port);
    const std::string url = authorize_url(client_id, port, code_challenge_s256(verifier), state);

    if (open_in_browser(url)) {
        set_status("Waiting for you to approve CallScribe in the browser...");
    } else {
        bool copied = util::copy_to_clipboard(url);
        std::lock_guard<std::mutex> lock(mutex_);
        manual_login_url_ = url;
        status_ = std::string("Couldn't open a browser. Open this link to log in") +
                  (copied ? " (copied to the clipboard):" : ":");
    }

    auto params = listener.wait(std::chrono::duration_cast<std::chrono::seconds>(kLoginTimeout), cancel_login_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        manual_login_url_.clear();
    }
    if (params.count("error")) {
        set_status("Spotify login refused: " + params["error"]);
        return;
    }
    // A mismatched state means this redirect isn't the answer to our request.
    if (params["state"] != state || params["code"].empty()) {
        set_status("Spotify login failed: the browser came back with an unexpected response.");
        return;
    }

    Response r = post_form(kTokenUrl, {{"grant_type", "authorization_code"},
                                       {"code", params["code"]},
                                       {"redirect_uri", redirect_uri(port)},
                                       {"client_id", client_id},
                                       {"code_verifier", verifier}});
    auto j = parse_or_empty(r.body);
    if (!is_success(r.status) || !j.contains("access_token") || !j.contains("refresh_token")) {
        set_status("Spotify login failed: " + error_message(r.body, r.status));
        return;
    }

    token_client_id_ = client_id;
    keyring_checked_ = true;
    access_token_ = j["access_token"].get<std::string>();
    access_expires_ = std::chrono::steady_clock::now() + std::chrono::seconds(j.value("expires_in", 3600) - 60);
    refresh_token_ = j["refresh_token"].get<std::string>();
    connected_.store(true);

    std::string error;
    if (store_refresh_token(client_id, refresh_token_, error))
        set_status("Connected.");
    else
        set_status("Connected for this run only -- " + error);
}

void SpotifyClient::do_play(const std::string& playlist) {
    if (playlist.empty()) return;
    const std::string uri = normalize_playlist_uri(playlist);
    if (uri.empty()) {
        set_status("Not a playlist link: " + playlist);
        return;
    }
    if (!ensure_refresh_token()) {
        set_status("Not connected -- Connect Spotify in Settings to play music while focusing.");
        return;
    }

    if (start_playback(nlohmann::json{{"context_uri", uri}}.dump())) set_status("Playing your focus playlist.");
    fetch_now_playing_after_command();
}

bool SpotifyClient::start_playback(const std::string& body) {
    HttpResponse r = api("PUT", "/me/player/play", body);

    // 404 = no active device. Spotify only plays somewhere that's already
    // open, but it doesn't have to be playing: hand it the first device the
    // account can see (this computer's app, a phone, a speaker).
    if (r.status == 404) {
        HttpResponse devices = api("GET", "/me/player/devices");
        auto j = parse_or_empty(devices.body);
        std::string device_id;
        if (j.contains("devices") && j["devices"].is_array()) {
            for (const auto& d : j["devices"]) {
                if (d.value("is_restricted", false) || !d.contains("id") || !d["id"].is_string()) continue;
                device_id = d["id"].get<std::string>();
                break;
            }
        }
        if (device_id.empty()) {
            set_status("No Spotify device is open -- start Spotify on this computer or your phone.");
            return false;
        }
        r = api("PUT", "/me/player/play?device_id=" + url_encode(device_id), body);
    }

    if (is_success(r.status)) return true;
    if (r.status != 401)  // 401 has already set its own message
        set_status("Couldn't start playback: " + error_message(r.body, r.status));
    return false;
}

void SpotifyClient::do_toggle(const std::string& fallback_playlist) {
    if (!ensure_refresh_token()) return;
    // Asked fresh rather than trusting the cached state: the user may have
    // paused from their phone since the last fetch.
    HttpResponse state = api("GET", "/me/player");
    const auto j = parse_or_empty(state.body);
    if (state.status == 200 && j.value("is_playing", false)) {
        HttpResponse r = api("PUT", "/me/player/pause");
        if (!is_success(r.status) && r.status != 401)
            set_status("Couldn't pause: " + error_message(r.body, r.status));
    } else {
        // Nothing loaded anywhere: resuming would have nothing to resume.
        const bool nothing_loaded = state.status != 200 || !j.contains("item") || j["item"].is_null();
        const std::string uri = normalize_playlist_uri(fallback_playlist);
        if (nothing_loaded && !uri.empty())
            start_playback(nlohmann::json{{"context_uri", uri}}.dump());
        else
            start_playback("");
    }
    fetch_now_playing_after_command();
}

void SpotifyClient::do_pause() {
    if (!ensure_refresh_token()) return;
    HttpResponse r = api("PUT", "/me/player/pause");
    // 403 here usually means "already paused", 404 that nothing is open.
    // Neither is worth a message: the music is not playing, which was the goal.
    if (is_success(r.status)) set_status("Paused for the break.");
    fetch_now_playing_after_command();
}

}  // namespace callscribe::spotify
