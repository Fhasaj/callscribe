#pragma once
#include <string>

namespace callscribe::util {

// Copies `text` to the system clipboard via wl-copy (Wayland) or xclip/xsel
// (X11), whichever is found first. Returns false if none is available.
bool copy_to_clipboard(const std::string& text);

// Reads the system clipboard via wl-paste (Wayland) or xclip/xsel (X11),
// whichever is found first. Returns an empty string if none is available or
// the clipboard is empty.
std::string paste_from_clipboard();

}  // namespace callscribe::util
