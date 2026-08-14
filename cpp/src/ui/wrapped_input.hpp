#pragma once
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>

#include <string>

namespace callscribe::ui {

// A text box that word-wraps its content instead of scrolling it sideways.
//
// FTXUI's own Input renders each logical line as a single unbreakable text()
// inside a frame, so a long paragraph slides out of view to the right and only
// the part around the cursor is readable. This lays the content out with
// WrapLayout() -- the same wrapper the read-only panes use -- and moves the
// cursor by *visual* line, so arrows and clicks land where they look like they
// should. The content itself is never modified: no line breaks are inserted,
// so what a Copy button puts on the clipboard is still what was typed.
//
// Single-line options (multiline = false) are passed straight to ftxui::Input:
// those fields are one row tall inside a horizontal layout, where scrolling is
// the right behaviour.
ftxui::Component WrappedInput(std::string* content, std::string placeholder,
                              ftxui::InputOption option = {});

}  // namespace callscribe::ui
