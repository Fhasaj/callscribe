#include "util/keyring.hpp"

#include <libsecret/secret.h>

namespace callscribe::util {

namespace {

const SecretSchema* secret_schema() {
    static const SecretSchema schema = {
        "com.callscribe.Secret",
        SECRET_SCHEMA_NONE,
        {
            {"name", SECRET_SCHEMA_ATTRIBUTE_STRING},
            {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING},
        },
    };
    return &schema;
}

std::string take_error(GError* error) {
    std::string msg = (error && error->message) ? error->message : "unknown keyring error";
    if (error) g_error_free(error);
    return "Keyring: " + msg;
}

}  // namespace

bool store_secret(const std::string& name, const std::string& value, std::string& error) {
    GError* gerror = nullptr;
    const std::string label = "CallScribe " + name;
    gboolean ok = secret_password_store_sync(secret_schema(), SECRET_COLLECTION_DEFAULT, label.c_str(), value.c_str(),
                                             nullptr, &gerror, "name", name.c_str(), nullptr);
    if (!ok || gerror) {
        error = take_error(gerror);
        return false;
    }
    return true;
}

std::optional<std::string> lookup_secret(const std::string& name, std::string& error) {
    GError* gerror = nullptr;
    gchar* secret = secret_password_lookup_sync(secret_schema(), nullptr, &gerror, "name", name.c_str(), nullptr);
    if (gerror) {
        error = take_error(gerror);
        return std::nullopt;
    }
    if (!secret) return std::nullopt;
    std::string out = secret;
    secret_password_free(secret);
    return out;
}

bool clear_secret(const std::string& name, std::string& error) {
    GError* gerror = nullptr;
    secret_password_clear_sync(secret_schema(), nullptr, &gerror, "name", name.c_str(), nullptr);
    if (gerror) {
        error = take_error(gerror);
        return false;
    }
    return true;
}

}  // namespace callscribe::util
