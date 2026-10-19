#pragma once
#include <map>
#include <string>

// Pure helpers for Spotify's Authorization Code + PKCE login (RFC 7636): no
// network, no threads, so they can be tested on their own.
namespace callscribe::spotify {

// `length` characters from the RFC 7636 unreserved set, drawn from OpenSSL's
// CSPRNG. Used for the code verifier (43-128 chars) and the `state` value.
std::string random_token(size_t length);

// BASE64URL(SHA256(verifier)), unpadded -- the S256 code_challenge.
std::string code_challenge_s256(const std::string& verifier);

std::string base64url_encode(const unsigned char* data, size_t size);

// Percent-encodes everything but the unreserved set, for query strings and
// application/x-www-form-urlencoded bodies alike.
std::string url_encode(const std::string& value);
// The inverse, also turning '+' into a space as form encoding does.
std::string url_decode(const std::string& value);
// "a=1&b=two" -> {a: 1, b: two}, decoded. A leading '?' is skipped.
std::map<std::string, std::string> parse_query(const std::string& query);

std::string redirect_uri(int port);

std::string authorize_url(const std::string& client_id, int port, const std::string& challenge,
                          const std::string& state);

// Accepts a "spotify:playlist:<id>" URI or an open.spotify.com/playlist/<id>
// link (with or without ?si=..., intl-xx/ prefix) and returns the URI form the
// Web API wants. Returns "" for anything else.
std::string normalize_playlist_uri(const std::string& input);

}  // namespace callscribe::spotify
