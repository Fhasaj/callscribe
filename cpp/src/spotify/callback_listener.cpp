#include "spotify/callback_listener.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

#include "spotify/pkce.hpp"

namespace callscribe::spotify {

namespace {

// Sent before the code has been exchanged, so it can't claim success yet --
// Settings shows how the login ended.
constexpr const char* kDonePage =
    "<!doctype html><html><head><meta charset=\"utf-8\"><title>CallScribe</title></head>"
    "<body style=\"font-family:sans-serif;margin:3em\"><h2>Back to CallScribe</h2>"
    "<p>You can close this tab. CallScribe's Settings screen shows whether Spotify is connected.</p></body></html>";

void send_response(int client, const char* status, const std::string& body) {
    std::string response = std::string("HTTP/1.1 ") + status +
                           "\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\nContent-Length: " +
                           std::to_string(body.size()) + "\r\n\r\n" + body;
    size_t sent = 0;
    while (sent < response.size()) {
        ssize_t n = ::send(client, response.data() + sent, response.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) return;
        sent += static_cast<size_t>(n);
    }
}

// Reads up to the end of the request headers. A real browser sends them in one
// go; the cap and the poll timeout stop a stray client from wedging the wait.
std::string read_request_head(int client) {
    std::string head;
    char buf[1024];
    while (head.find("\r\n\r\n") == std::string::npos && head.size() < 8192) {
        pollfd p{client, POLLIN, 0};
        if (::poll(&p, 1, 2000) <= 0) break;
        ssize_t n = ::recv(client, buf, sizeof(buf), 0);
        if (n <= 0) break;
        head.append(buf, static_cast<size_t>(n));
    }
    return head;
}

}  // namespace

CallbackListener::CallbackListener(int port) {
    fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd_ < 0) throw std::runtime_error(std::string("socket: ") + std::strerror(errno));

    int yes = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 || ::listen(fd_, 4) < 0) {
        std::string msg = std::strerror(errno);
        ::close(fd_);
        fd_ = -1;
        throw std::runtime_error("Can't listen on 127.0.0.1:" + std::to_string(port) + " (" + msg +
                                 ") -- pick another port, and update the redirect URI to match.");
    }
}

CallbackListener::~CallbackListener() {
    if (fd_ >= 0) ::close(fd_);
}

std::map<std::string, std::string> CallbackListener::wait(std::chrono::seconds timeout,
                                                          const std::atomic<bool>& cancel) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!cancel.load()) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("Timed out waiting for the browser to come back from Spotify.");

        pollfd p{fd_, POLLIN, 0};
        int ready = ::poll(&p, 1, 500);
        if (ready < 0 && errno != EINTR) throw std::runtime_error(std::string("poll: ") + std::strerror(errno));
        if (ready <= 0) continue;

        int client = ::accept4(fd_, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0) continue;

        // "GET /callback?code=...&state=... HTTP/1.1"
        std::string head = read_request_head(client);
        std::string line = head.substr(0, head.find("\r\n"));
        size_t path_start = line.find(' ');
        size_t path_end = line.find(' ', path_start == std::string::npos ? 0 : path_start + 1);
        std::string target = (path_start == std::string::npos || path_end == std::string::npos)
                                 ? ""
                                 : line.substr(path_start + 1, path_end - path_start - 1);
        std::string path = target.substr(0, target.find('?'));

        if (path != "/callback") {
            send_response(client, "404 Not Found", "");
            ::close(client);
            continue;
        }
        size_t q = target.find('?');
        auto params = parse_query(q == std::string::npos ? "" : target.substr(q + 1));
        send_response(client, "200 OK", kDonePage);
        ::close(client);
        return params;
    }
    throw std::runtime_error("Spotify login cancelled.");
}

}  // namespace callscribe::spotify
