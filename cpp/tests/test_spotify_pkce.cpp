#include <doctest/doctest.h>

#include "spotify/pkce.hpp"

using namespace callscribe::spotify;

TEST_CASE("pkce: S256 challenge matches the RFC 7636 appendix B example") {
    CHECK(code_challenge_s256("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk") ==
          "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
}

TEST_CASE("pkce: base64url is unpadded and URL-safe") {
    const unsigned char bytes[] = {0xfb, 0xff, 0xbf};
    CHECK(base64url_encode(bytes, 3) == "-_-_");
    const unsigned char one[] = {'f'};
    CHECK(base64url_encode(one, 1) == "Zg");
    const unsigned char two[] = {'f', 'o'};
    CHECK(base64url_encode(two, 2) == "Zm8");
}

TEST_CASE("pkce: random tokens have the requested length and only unreserved characters") {
    std::string token = random_token(64);
    CHECK(token.size() == 64);
    CHECK(token.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~") ==
          std::string::npos);
    CHECK(random_token(64) != token);
}

TEST_CASE("pkce: url encoding round-trips") {
    CHECK(url_encode("a b&c=d/é") == "a%20b%26c%3Dd%2F%C3%A9");
    CHECK(url_decode("a%20b+c%26") == "a b c&");
    CHECK(url_decode(url_encode("http://127.0.0.1:8888/callback")) == "http://127.0.0.1:8888/callback");
}

TEST_CASE("pkce: callback query parsing") {
    auto q = parse_query("?code=AQB%2Fx&state=abc&flag");
    CHECK(q["code"] == "AQB/x");
    CHECK(q["state"] == "abc");
    CHECK(q.count("flag") == 1);
}

TEST_CASE("pkce: authorize URL carries the PKCE and redirect parameters") {
    std::string url = authorize_url("client123", 8888, "challenge", "state42");
    CHECK(url.rfind("https://accounts.spotify.com/authorize?", 0) == 0);
    CHECK(url.find("client_id=client123") != std::string::npos);
    CHECK(url.find("code_challenge_method=S256") != std::string::npos);
    CHECK(url.find("redirect_uri=http%3A%2F%2F127.0.0.1%3A8888%2Fcallback") != std::string::npos);
    CHECK(url.find("state=state42") != std::string::npos);
}

TEST_CASE("pkce: playlist links normalize to URIs") {
    CHECK(normalize_playlist_uri("spotify:playlist:37i9dQZF1DXcBWIGoYBM5M") == "spotify:playlist:37i9dQZF1DXcBWIGoYBM5M");
    CHECK(normalize_playlist_uri("https://open.spotify.com/playlist/37i9dQZF1DXcBWIGoYBM5M?si=abc123") ==
          "spotify:playlist:37i9dQZF1DXcBWIGoYBM5M");
    CHECK(normalize_playlist_uri("  https://open.spotify.com/intl-de/playlist/37i9dQZF1DXcBWIGoYBM5M  ") ==
          "spotify:playlist:37i9dQZF1DXcBWIGoYBM5M");
    CHECK(normalize_playlist_uri("https://open.spotify.com/album/1DFixLWuPkv3KT3TnV35m3").empty());
    CHECK(normalize_playlist_uri("spotify:playlist:bad id").empty());
    CHECK(normalize_playlist_uri("hello").empty());
}
