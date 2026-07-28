#pragma once
#include <string>

namespace callscribe::util {

// Copies `text` to the system clipboard via wl-copy (Wayland) or xclip/xsel
// (X11), whichever is found first. Returns false if none is available.
bool copy_to_clipboard(const std::string& text);

}  // namespace callscribe::util
