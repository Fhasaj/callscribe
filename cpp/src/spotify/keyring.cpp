#include "spotify/keyring.hpp"

#include <libsecret/secret.h>

namespace callscribe::spotify {

namespace {

// Keyed by client ID as well, so pointing the app at a different Spotify app
// can never hand it a token that was issued to another one.
const SecretSchema* token_schema() {
    static const SecretSchema schema = {
        "com.callscribe.SpotifyRefreshToken",
        SECRET_SCHEMA_NONE,
        {
            {"service", SECRET_SCHEMA_ATTRIBUTE_STRING},
            {"client_id", SECRET_SCHEMA_ATTRIBUTE_STRING},
            {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING},
        },
    };
    return &schema;
}

constexpr const char* kService = "spotify";

std::string take_error(GError* error) {
    std::string msg = (error && error->message) ? error->message : "unknown keyring error";
    if (error) g_error_free(error);
    return "Keyring: " + msg;
}

}  // namespace

bool store_refresh_token(const std::string& client_id, const std::string& token, std::string& error) {
    GError* gerror = nullptr;
    gboolean ok = secret_password_store_sync(token_schema(), SECRET_COLLECTION_DEFAULT,
                                             "CallScribe Spotify refresh token", token.c_str(), nullptr, &gerror,
                                             "service", kService, "client_id", client_id.c_str(), nullptr);
    if (!ok || gerror) {
        error = take_error(gerror);
        return false;
    }
    return true;
}

std::optional<std::string> lookup_refresh_token(const std::string& client_id, std::string& error) {
    GError* gerror = nullptr;
    gchar* secret = secret_password_lookup_sync(token_schema(), nullptr, &gerror, "service", kService, "client_id",
                                                client_id.c_str(), nullptr);
    if (gerror) {
        error = take_error(gerror);
        return std::nullopt;
    }
    if (!secret) return std::nullopt;
    std::string out = secret;
    // Wipes the copy libsecret handed us; ours is a plain std::string, which is
    // as far as this goes.
    secret_password_free(secret);
    return out;
}

bool clear_refresh_token(const std::string& client_id, std::string& error) {
    GError* gerror = nullptr;
    secret_password_clear_sync(token_schema(), nullptr, &gerror, "service", kService, "client_id",
                               client_id.c_str(), nullptr);
    if (gerror) {
        error = take_error(gerror);
        return false;
    }
    return true;
}

}  // namespace callscribe::spotify
