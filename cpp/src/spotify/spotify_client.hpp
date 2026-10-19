#pragma once
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "util/blocking_queue.hpp"

namespace callscribe::spotify {

// What the account's player is doing, as of the last fetch.
struct NowPlaying {
    bool active = false;  // false: no device has anything loaded
    bool playing = false;
    std::string track;
    std::string artists;
    std::string device;
    int progress_ms = 0;
    int duration_ms = 0;
    std::chrono::steady_clock::time_point fetched_at{};

    // Moves the fetched position on by the time since, while playing, so a
    // progress bar keeps going between fetches.
    int progress_now_ms(std::chrono::steady_clock::time_point now) const;
};

// Optional Spotify control for the focus timer: log in once (Authorization
// Code + PKCE in the browser), then start a playlist when a focus round starts
// and pause it for the breaks.
//
// Every public call returns immediately. The work -- HTTP, the keyring, waiting
// for the browser -- runs in order on one worker thread, and the outcome is
// reported through status()/connected() for the UI to poll. With no client ID
// configured every request is a silent no-op, so the rest of the app never has
// to ask whether Spotify is set up.
class SpotifyClient {
public:
    SpotifyClient();
    ~SpotifyClient();
    SpotifyClient(const SpotifyClient&) = delete;
    SpotifyClient& operator=(const SpotifyClient&) = delete;

    // Changing the client ID drops any session held for the previous one.
    void configure(const std::string& client_id, int redirect_port);

    bool configured() const;
    // A refresh token is known (in the keyring, or at least in memory).
    bool connected() const { return connected_.load(); }
    // A browser login is waiting to come back.
    bool logging_in() const { return logging_in_.load(); }
    std::string status() const;
    // Set only when the browser could not be opened: the link to open by hand.
    std::string manual_login_url() const;

    // Looks for a stored refresh token, without touching the network.
    void check();
    void connect();
    void cancel_login() { cancel_login_.store(true); }
    // Forgets the token, in the keyring too.
    void disconnect();
    // `playlist` as configured: a spotify: URI or an open.spotify.com link.
    void play_playlist(const std::string& playlist);
    void pause();

    // Mini-player controls. toggle_playback() resumes where the account left
    // off; with nothing to resume it starts `fallback_playlist`, if given.
    void toggle_playback(const std::string& fallback_playlist);
    void next_track();
    void previous_track();

    NowPlaying now_playing() const;
    // Fetches the player state unless one was asked for within `min_interval`
    // or is still queued -- safe to call on every frame.
    void refresh_now_playing(std::chrono::milliseconds min_interval = std::chrono::seconds(5));

private:
    struct HttpResponse {
        long status = 0;
        std::string body;
    };

    void enqueue(std::function<void()> job);
    void set_status(std::string status);

    // Worker thread only, from here down.
    void do_login();
    void do_play(const std::string& playlist);
    void do_pause();
    void do_toggle(const std::string& fallback_playlist);
    // PUT /me/player/play, falling back to the first available device when
    // none is active. `body` empty resumes; otherwise it names what to play.
    // Returns whether playback started; sets the status if it didn't.
    bool start_playback(const std::string& body);
    void fetch_now_playing();
    // Lets Spotify apply a command before the player state is read back --
    // asked immediately, it still reports the old track.
    void fetch_now_playing_after_command();
    // Drops tokens that belong to a different client ID, then loads the refresh
    // token from the keyring if none is held yet. False if there isn't one.
    bool ensure_refresh_token();
    bool refresh_access_token();
    void forget_session(const std::string& reason);
    // A Web API call with a valid access token, refreshed and retried once on a
    // 401. Throws on transport errors.
    HttpResponse api(const std::string& method, const std::string& path, const std::string& body = "");

    mutable std::mutex mutex_;
    std::string client_id_;
    int redirect_port_ = 8888;
    std::string status_;
    std::string manual_login_url_;
    NowPlaying now_playing_;
    std::chrono::steady_clock::time_point last_fetch_request_{};
    bool fetch_queued_ = false;

    std::atomic<bool> connected_{false};
    std::atomic<bool> logging_in_{false};
    std::atomic<bool> cancel_login_{false};
    std::atomic<bool> stopping_{false};

    // Worker-owned session state.
    std::string token_client_id_;  // which client ID the tokens below belong to
    bool keyring_checked_ = false;
    std::string refresh_token_;
    std::string access_token_;
    std::chrono::steady_clock::time_point access_expires_{};

    util::BlockingQueue<std::function<void()>> jobs_;
    std::thread worker_;
};

}  // namespace callscribe::spotify
