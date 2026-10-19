#pragma once
#include <optional>
#include <string>

// Small secrets (API keys) in the desktop keyring through libsecret, keyed by
// a name -- the same Secret Service the Spotify token uses (spotify/keyring.hpp
// keeps its own schema). KWallet on Plasma, GNOME Keyring on GNOME.
//
// Synchronous D-Bus calls that can wait on the user unlocking their wallet:
// keep them off hot paths.
namespace callscribe::util {

// Each returns false / nullopt and fills `error` on failure. A lookup that
// finds nothing returns nullopt with `error` left empty.
bool store_secret(const std::string& name, const std::string& value, std::string& error);
std::optional<std::string> lookup_secret(const std::string& name, std::string& error);
bool clear_secret(const std::string& name, std::string& error);

}  // namespace callscribe::util
