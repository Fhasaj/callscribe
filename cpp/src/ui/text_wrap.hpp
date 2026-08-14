#pragma once
#include <ftxui/dom/elements.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace callscribe::ui {

// One visual line produced by wrapping: the source bytes [begin, end) of the
// content, plus the indent (in spaces) the line is rendered with. `soft_break`
// marks a line that was only broken because it ran out of width -- the source
// continues on the next visual line without a real '\n' between them.
struct WrapSegment {
    std::size_t begin = 0;
    std::size_t end = 0;
    int indent = 0;
    bool soft_break = false;
};

// Lays `content` out in `width` columns. Blank lines and leading indentation
// are preserved, and continuation lines inherit their source line's indent so
// bullet lists stay readable. A width of 1 or less means "not measured yet"
// (first frame) and yields the source lines untouched.
std::vector<WrapSegment> WrapLayout(const std::string& content, int width);

// The text of one segment as it is rendered, indent included.
std::string SegmentText(const std::string& content, const WrapSegment& segment);

// Word-wraps `content` to `width` columns, returning the wrapped text with a
// '\n' at every break. Every break it inserts is remembered for
// UnwrapSelection().
std::string WrapText(const std::string& content, int width);

// How many lines WrapText(content, width) produces. Used to size scroll steps.
int WrappedLineCount(const std::string& content, int width);

// `content` wrapped to `width` and rendered as a single multi-line text().
//
// Deliberately not ftxui::paragraph(): paragraph lays every word out as its own
// element separated by a flexbox gap, and those gap columns hold no character.
// FTXUI's selection only collects characters, so drag-selecting a paragraph
// copies "thequickfox" -- and the original line breaks are gone too. One
// multi-line text() node keeps the spaces and the newlines real, so what gets
// copied matches what is on screen.
ftxui::Element WrappedText(const std::string& content, int width);

// Undoes the line breaks that wrapping inserted into `selection`.
//
// A drag-selection is collected from the screen, so it comes back broken at
// whatever column the pane happened to be wide -- pasting it elsewhere gives a
// ragged block instead of flowing paragraphs. WrapText records which of the
// lines it emitted ended in a soft break; this rejoins exactly those and leaves
// the author's own line breaks (blank lines, bullets, sign-offs) alone.
std::string UnwrapSelection(const std::string& selection);

}  // namespace callscribe::ui
