#include "ui/text_wrap.hpp"

#include <ftxui/screen/string.hpp>

#include <algorithm>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>

namespace callscribe::ui {

namespace {

bool is_space(char c) { return c == ' ' || c == '\t'; }

// Byte offset of the code point after the one starting at `i`. Continuation
// bytes are 10xxxxxx, so skipping them lands on the next code point.
size_t next_code_point(const std::string& s, size_t i) {
    if (i >= s.size()) return s.size();
    ++i;
    while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
    return i;
}

int span_width(const std::string& s, size_t begin, size_t end) {
    return ftxui::string_width(std::string_view(s).substr(begin, end - begin));
}

// Lays out a single source line (no '\n' inside), appending its visual lines.
void layout_line(const std::string& content, size_t line_begin, size_t line_end, int width,
                 std::vector<WrapSegment>& out) {
    size_t first = line_begin;
    while (first < line_end && is_space(content[first])) ++first;

    // Blank (or whitespace-only) lines are paragraph breaks -- keep them as an
    // empty line rather than folding them away.
    if (first == line_end) {
        out.push_back({line_begin, line_begin, 0, false});
        return;
    }

    // Tabs would throw the column arithmetic off, so the indent is measured in
    // characters and re-rendered as spaces; continuation lines reuse it.
    int indent = static_cast<int>(first - line_begin);
    if (indent >= width) indent = 0;
    const int body_width = std::max(1, width - indent);

    size_t seg_begin = first;
    size_t seg_end = first;
    int seg_width = 0;
    bool has_word = false;

    auto flush = [&](bool soft) {
        out.push_back({seg_begin, seg_end, indent, soft});
        has_word = false;
        seg_width = 0;
    };

    size_t pos = first;
    while (pos < line_end) {
        const size_t gap_begin = pos;
        while (pos < line_end && is_space(content[pos])) ++pos;
        // Interior spacing is kept verbatim (each character one column), so
        // aligned columns in a transcript survive the wrap.
        const int gap = static_cast<int>(pos - gap_begin);

        const size_t word_begin = pos;
        while (pos < line_end && !is_space(content[pos])) ++pos;
        const size_t word_end = pos;
        if (word_begin == word_end) break;

        const int word_width = span_width(content, word_begin, word_end);

        // A word longer than the line gets hard-split on glyph boundaries --
        // otherwise it would overflow the pane and be clipped away.
        if (word_width > body_width) {
            if (has_word) flush(true);
            size_t chunk_begin = word_begin;
            int chunk_width = 0;
            for (size_t g = word_begin; g < word_end;) {
                const size_t next = next_code_point(content, g);
                const int glyph_width = std::max(1, span_width(content, g, next));
                if (chunk_width + glyph_width > body_width) {
                    seg_begin = chunk_begin;
                    seg_end = g;
                    flush(true);
                    chunk_begin = g;
                    chunk_width = 0;
                }
                chunk_width += glyph_width;
                g = next;
            }
            seg_begin = chunk_begin;
            seg_end = word_end;
            seg_width = chunk_width;
            has_word = true;
            continue;
        }

        if (has_word && seg_width + gap + word_width > body_width) {
            flush(true);
        }
        if (has_word) {
            seg_width += gap + word_width;
        } else {
            seg_begin = word_begin;
            seg_width = word_width;
        }
        seg_end = word_end;
        has_word = true;
    }

    if (has_word) flush(false);
}

// Lines that WrapText broke itself, so UnwrapSelection can tell them from the
// author's own line breaks. Only ever read/written from the UI thread, but the
// mutex costs nothing next to a render and keeps that assumption cheap to hold.
std::mutex g_soft_mutex;
std::unordered_set<std::string> g_soft_lines;

void remember_soft_break(std::string line) {
    if (line.empty()) return;
    std::lock_guard<std::mutex> lock(g_soft_mutex);
    // Every visible pane re-wraps on every render, so the set would otherwise
    // grow without bound over a long session. The lines it holds are rebuilt by
    // the next render, so dropping them all is safe.
    if (g_soft_lines.size() > 20000) g_soft_lines.clear();
    g_soft_lines.insert(std::move(line));
}

bool was_soft_break(const std::string& line) {
    if (line.empty()) return false;
    std::lock_guard<std::mutex> lock(g_soft_mutex);
    return g_soft_lines.count(line) > 0;
}

std::string rtrim(const std::string& s) {
    size_t end = s.size();
    while (end > 0 && is_space(s[end - 1])) --end;
    return s.substr(0, end);
}

std::string ltrim(const std::string& s) {
    size_t begin = 0;
    while (begin < s.size() && is_space(s[begin])) ++begin;
    return s.substr(begin);
}

}  // namespace

std::vector<WrapSegment> WrapLayout(const std::string& content, int width) {
    std::vector<WrapSegment> out;
    // A width of 1 means the pane hasn't been laid out yet (first frame). Emit
    // the source lines untouched; the next frame wraps them for real.
    const bool measured = width > 1;

    size_t start = 0;
    while (true) {
        const size_t nl = content.find('\n', start);
        size_t line_end = nl == std::string::npos ? content.size() : nl;
        if (line_end > start && content[line_end - 1] == '\r') --line_end;
        if (measured) {
            layout_line(content, start, line_end, width, out);
        } else {
            out.push_back({start, line_end, 0, false});
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    if (out.empty()) out.push_back({0, 0, 0, false});
    return out;
}

std::string SegmentText(const std::string& content, const WrapSegment& segment) {
    std::string out(static_cast<size_t>(segment.indent), ' ');
    out.append(content, segment.begin, segment.end - segment.begin);
    return out;
}

std::string WrapText(const std::string& content, int width) {
    const auto segments = WrapLayout(content, width);
    std::string out;
    for (size_t i = 0; i < segments.size(); ++i) {
        if (i > 0) out += '\n';
        std::string line = SegmentText(content, segments[i]);
        if (segments[i].soft_break) remember_soft_break(line);
        out += line;
    }
    return out;
}

int WrappedLineCount(const std::string& content, int width) {
    return std::max(1, static_cast<int>(WrapLayout(content, width).size()));
}

ftxui::Element WrappedText(const std::string& content, int width) {
    return ftxui::text(WrapText(content, width));
}

std::string UnwrapSelection(const std::string& selection) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (true) {
        const size_t nl = selection.find('\n', start);
        lines.push_back(selection.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
        if (nl == std::string::npos) break;
        start = nl + 1;
    }

    std::string out;
    bool joined_previous = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string line = lines[i];
        // The screen pads selected rows out to the drag rectangle, so compare
        // (and rejoin) on the trimmed text.
        const std::string trimmed = rtrim(line);
        const bool soft = i + 1 < lines.size() && was_soft_break(trimmed);

        if (joined_previous) {
            line = ltrim(line);
            if (!out.empty() && out.back() != ' ') out += ' ';
        }
        out += soft ? rtrim(line) : line;
        if (!soft && i + 1 < lines.size()) out += '\n';
        joined_previous = soft;
    }
    return out;
}

}  // namespace callscribe::ui
