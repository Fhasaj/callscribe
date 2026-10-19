#pragma once
#include <optional>
#include <string>

// The Spotify refresh token, kept in the desktop keyring through libsecret's
// Secret Service API rather than on disk. On KDE Plasma that is KWallet
// (ksecretd answers org.freedesktop.secrets), on GNOME/Fedora Workstation it is
// GNOME Keyring -- libsecret talks to whichever owns the bus name.
//
// Every call is a synchronous D-Bus round trip that can also block on the user
// unlocking their wallet, so never call these from the UI thread.
namespace callscribe::spotify {

// Each returns false / nullopt and fills `error` when no Secret Service is
// reachable or the request fails. A lookup that simply finds nothing returns
// nullopt with `error` left empty.
bool store_refresh_token(const std::string& client_id, const std::string& token, std::string& error);
std::optional<std::string> lookup_refresh_token(const std::string& client_id, std::string& error);
bool clear_refresh_token(const std::string& client_id, std::string& error);

}  // namespace callscribe::spotify
