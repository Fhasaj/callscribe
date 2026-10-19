#pragma once
#include <atomic>
#include <chrono>
#include <map>
#include <string>

namespace callscribe::spotify {

// The tiny HTTP server the browser is redirected back to at the end of the
// Spotify login: http://127.0.0.1:<port>/callback?code=...&state=...
//
// Bound to the loopback address only. Opened before the browser is, so a port
// already in use is reported up front instead of the login silently dead-ending.
class CallbackListener {
public:
    // Throws std::runtime_error if the port can't be bound.
    explicit CallbackListener(int port);
    ~CallbackListener();
    CallbackListener(const CallbackListener&) = delete;
    CallbackListener& operator=(const CallbackListener&) = delete;

    // Blocks until a request for /callback arrives and returns its decoded
    // query parameters. Anything else the browser asks for (a favicon) gets a
    // 404 and the wait carries on. Throws on timeout, or once `cancel` is set
    // -- checked every half second.
    std::map<std::string, std::string> wait(std::chrono::seconds timeout, const std::atomic<bool>& cancel);

private:
    int fd_ = -1;
};

}  // namespace callscribe::spotify
