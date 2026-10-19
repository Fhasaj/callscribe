#include "spotify/pkce.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <cctype>
#include <stdexcept>

namespace callscribe::spotify {

namespace {

constexpr const char* kScopes = "user-modify-playback-state user-read-playback-state";

bool is_unreserved(unsigned char c) { return std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~'; }

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool is_spotify_id(const std::string& id) {
    if (id.empty()) return false;
    for (unsigned char c : id) {
        if (!std::isalnum(c)) return false;
    }
    return true;
}

}  // namespace

std::string random_token(size_t length) {
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    constexpr size_t kAlphabetSize = sizeof(kAlphabet) - 1;  // 66
    // Rejection sampling: 256 isn't a multiple of 66, so bytes past the last
    // whole multiple would skew the distribution towards the first letters.
    constexpr unsigned kLimit = 256 - (256 % kAlphabetSize);

    std::string out;
    out.reserve(length);
    unsigned char buf[64];
    while (out.size() < length) {
        if (RAND_bytes(buf, sizeof(buf)) != 1) throw std::runtime_error("No randomness available for the Spotify login");
        for (unsigned char b : buf) {
            if (b >= kLimit) continue;
            out.push_back(kAlphabet[b % kAlphabetSize]);
            if (out.size() == length) break;
        }
    }
    return out;
}

std::string base64url_encode(const unsigned char* data, size_t size) {
    static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < size; i += 3) {
        unsigned n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out += kTable[(n >> 18) & 63];
        out += kTable[(n >> 12) & 63];
        out += kTable[(n >> 6) & 63];
        out += kTable[n & 63];
    }
    if (i + 1 == size) {
        unsigned n = data[i] << 16;
        out += kTable[(n >> 18) & 63];
        out += kTable[(n >> 12) & 63];
    } else if (i + 2 == size) {
        unsigned n = (data[i] << 16) | (data[i + 1] << 8);
        out += kTable[(n >> 18) & 63];
        out += kTable[(n >> 12) & 63];
        out += kTable[(n >> 6) & 63];
    }
    return out;
}

std::string code_challenge_s256(const std::string& verifier) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_size = 0;
    if (EVP_Digest(verifier.data(), verifier.size(), digest, &digest_size, EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("SHA-256 failed");
    return base64url_encode(digest, digest_size);
}

std::string url_encode(const std::string& value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : value) {
        if (is_unreserved(c)) {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        }
    }
    return out;
}

std::string url_decode(const std::string& value) {
    std::string out;
    for (size_t i = 0; i < value.size(); ++i) {
        char c = value[i];
        if (c == '+') {
            out += ' ';
        } else if (c == '%' && i + 2 < value.size() && hex_value(value[i + 1]) >= 0 && hex_value(value[i + 2]) >= 0) {
            out += static_cast<char>(hex_value(value[i + 1]) * 16 + hex_value(value[i + 2]));
            i += 2;
        } else {
            out += c;
        }
    }
    return out;
}

std::map<std::string, std::string> parse_query(const std::string& query) {
    std::map<std::string, std::string> out;
    size_t pos = (!query.empty() && query[0] == '?') ? 1 : 0;
    while (pos < query.size()) {
        size_t amp = query.find('&', pos);
        if (amp == std::string::npos) amp = query.size();
        std::string pair = query.substr(pos, amp - pos);
        size_t eq = pair.find('=');
        if (!pair.empty()) {
            if (eq == std::string::npos)
                out[url_decode(pair)] = "";
            else
                out[url_decode(pair.substr(0, eq))] = url_decode(pair.substr(eq + 1));
        }
        pos = amp + 1;
    }
    return out;
}

std::string redirect_uri(int port) { return "http://127.0.0.1:" + std::to_string(port) + "/callback"; }

std::string authorize_url(const std::string& client_id, int port, const std::string& challenge,
                          const std::string& state) {
    return "https://accounts.spotify.com/authorize?response_type=code&client_id=" + url_encode(client_id) +
           "&scope=" + url_encode(kScopes) + "&redirect_uri=" + url_encode(redirect_uri(port)) +
           "&code_challenge_method=S256&code_challenge=" + url_encode(challenge) + "&state=" + url_encode(state);
}

std::string normalize_playlist_uri(const std::string& input) {
    std::string s = input;
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();

    const std::string uri_prefix = "spotify:playlist:";
    if (s.rfind(uri_prefix, 0) == 0) {
        std::string id = s.substr(uri_prefix.size());
        return is_spotify_id(id) ? uri_prefix + id : "";
    }

    const std::string marker = "open.spotify.com/";
    size_t at = s.find(marker);
    if (at == std::string::npos) return "";
    std::string path = s.substr(at + marker.size());
    path = path.substr(0, path.find_first_of("?#"));
    // Localised share links: open.spotify.com/intl-de/playlist/<id>
    if (path.rfind("intl-", 0) == 0) {
        size_t slash = path.find('/');
        if (slash == std::string::npos) return "";
        path = path.substr(slash + 1);
    }
    const std::string playlist_prefix = "playlist/";
    if (path.rfind(playlist_prefix, 0) != 0) return "";
    std::string id = path.substr(playlist_prefix.size());
    if (!id.empty() && id.back() == '/') id.pop_back();
    return is_spotify_id(id) ? uri_prefix + id : "";
}

}  // namespace callscribe::spotify
