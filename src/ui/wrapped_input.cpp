#include "ui/wrapped_input.hpp"

#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/string.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

#include "ui/text_wrap.hpp"

namespace callscribe::ui {

namespace {

using namespace ftxui;

// UTF-8 continuation bytes are 10xxxxxx, so skipping them walks whole code
// points -- enough to keep the cursor off the middle of a multi-byte glyph.
size_t code_point_next(const std::string& s, size_t i) {
    if (i >= s.size()) return s.size();
    ++i;
    while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
    return i;
}

size_t code_point_previous(const std::string& s, size_t i) {
    if (i == 0) return 0;
    --i;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;
    return i;
}

int span_width(const std::string& s, size_t begin, size_t end) {
    if (end <= begin) return 0;
    return string_width(std::string_view(s).substr(begin, end - begin));
}

bool is_word_byte(char c) {
    auto u = static_cast<unsigned char>(c);
    return u >= 0x80 || std::isalnum(u) != 0 || c == '_';
}

class WrappedInputBase : public ComponentBase {
public:
    WrappedInputBase(std::string* content, std::string placeholder, InputOption option)
        : content_(content), placeholder_(std::move(placeholder)), option_(std::move(option)) {}

    bool Focusable() const override { return true; }

    Element OnRender() override {
        const bool is_focused = Focused();
        const auto focus_style = (!is_focused && !hovered_) ? focus
                                 : option_.insert()         ? focusCursorBarBlinking
                                                            : focusCursorBlockBlinking;
        auto transform = option_.transform ? option_.transform : InputOption::Default().transform;

        // The width comes from the previous frame -- 1 (i.e. "unwrapped") only
        // for the very first one, which the next render corrects.
        const int width = std::max(1, inner_box_.x_max - inner_box_.x_min + 1);

        if (content_->empty()) {
            segments_.clear();
            cursor_segment_ = 0;
            // Placeholders wrap too: the settings prompt boxes use a whole
            // default instruction as theirs.
            auto element = text(WrapText(placeholder_, width)) | focus_style | yframe | xflex |
                           reflect(inner_box_);
            return transform({std::move(element), hovered_, is_focused, true}) | xflex | reflect(box_);
        }

        clamp_cursor();
        segments_ = WrapLayout(*content_, width);
        cursor_segment_ = segment_of(cursor());

        Elements lines;
        lines.reserve(segments_.size());
        for (size_t i = 0; i < segments_.size(); ++i) {
            const auto& segment = segments_[i];
            const std::string line = SegmentText(*content_, segment);
            if (i != cursor_segment_) {
                lines.push_back(text(line));
                continue;
            }

            // Split the cursor's line around the glyph under the cursor. The
            // cursor cell reflects its own box: that is what maps a later mouse
            // click back onto a position in the text, whatever the frame has
            // scrolled to.
            const size_t offset = std::clamp(cursor(), segment.begin, segment.end);
            const size_t indent = static_cast<size_t>(segment.indent);
            const size_t local = indent + (offset - segment.begin);
            if (local >= line.size()) {
                lines.push_back(hbox({
                                    text(line),
                                    text(is_focused ? " " : "") | focus_style | reflect(cursor_box_),
                                }) |
                                xflex);
                continue;
            }
            const size_t glyph_end = code_point_next(line, local);
            lines.push_back(hbox({
                                text(line.substr(0, local)),
                                text(line.substr(local, glyph_end - local)) | focus_style |
                                    reflect(cursor_box_),
                                text(line.substr(glyph_end)),
                            }) |
                            xflex);
        }

        // yframe, not frame: the text is wrapped to the box, so there is never
        // anything to scroll to horizontally.
        auto element = vbox(std::move(lines)) | yframe | xflex | reflect(inner_box_);
        return transform({std::move(element), hovered_, is_focused, false}) | xflex | reflect(box_);
    }

    bool OnEvent(Event event) override {
        clamp_cursor();

        if (event.is_mouse()) return OnMouse(event);
        if (!Focused()) return false;

        if (event == Event::Return) return HandleReturn();
        if (event.is_character()) return HandleCharacter(event.character());
        if (event == Event::Backspace) return HandleBackspace();
        if (event == Event::Delete) return HandleDelete();
        if (event == Event::ArrowLeft) return MoveTo(code_point_previous(*content_, cursor()));
        if (event == Event::ArrowRight) return MoveTo(code_point_next(*content_, cursor()));
        if (event == Event::ArrowUp) return MoveVertically(-1);
        if (event == Event::ArrowDown) return MoveVertically(1);
        if (event == Event::Home) return MoveTo(line_begin());
        if (event == Event::End) return MoveTo(line_end());
        if (event == Event::ArrowLeftCtrl) return HandleWordLeft();
        if (event == Event::ArrowRightCtrl) return HandleWordRight();
        if (event == Event::Insert) {
            option_.insert() = !option_.insert();
            return true;
        }
        return false;
    }

private:
    size_t cursor() const { return static_cast<size_t>(std::max(0, option_.cursor_position())); }
    void set_cursor(size_t position) {
        option_.cursor_position() = static_cast<int>(std::min(position, content_->size()));
    }
    void clamp_cursor() { set_cursor(cursor()); }

    void notify_change() {
        if (option_.on_change) option_.on_change();
    }

    // The visual line holding `position`: the first segment that reaches it.
    // A position inside whitespace that the wrap swallowed belongs to the line
    // before it, which is where the cursor is drawn.
    size_t segment_of(size_t position) const {
        if (segments_.empty()) return 0;
        for (size_t i = 0; i < segments_.size(); ++i) {
            if (position <= segments_[i].end) return i;
        }
        return segments_.size() - 1;
    }

    int column_of(size_t position) const {
        if (segments_.empty()) return 0;
        const auto& segment = segments_[segment_of(position)];
        const size_t offset = std::clamp(position, segment.begin, segment.end);
        return segment.indent + span_width(*content_, segment.begin, offset);
    }

    // The byte offset on `segment` that renders at (or just before) `column`.
    size_t offset_at(const WrapSegment& segment, int column) const {
        int at = segment.indent;
        size_t offset = segment.begin;
        while (offset < segment.end) {
            const size_t next = code_point_next(*content_, offset);
            const int width = std::max(1, span_width(*content_, offset, next));
            if (at + width > column) break;
            at += width;
            offset = next;
        }
        return offset;
    }

    size_t line_begin() const {
        if (segments_.empty()) return 0;
        return segments_[segment_of(cursor())].begin;
    }

    size_t line_end() const {
        if (segments_.empty()) return content_->size();
        return segments_[segment_of(cursor())].end;
    }

    bool MoveTo(size_t position) {
        const size_t before = cursor();
        set_cursor(position);
        return cursor() != before;
    }

    bool MoveVertically(int delta) {
        if (segments_.empty()) return false;
        const int column = column_of(cursor());
        const int target = static_cast<int>(segment_of(cursor())) + delta;
        if (target < 0 || target >= static_cast<int>(segments_.size())) {
            // Already on the first/last line -- go to its start/end instead, so
            // the key still does something and doesn't leak to another widget.
            return MoveTo(delta < 0 ? segments_.front().begin : segments_.back().end);
        }
        set_cursor(offset_at(segments_[static_cast<size_t>(target)], column));
        return true;
    }

    bool HandleReturn() {
        if (option_.multiline()) HandleCharacter("\n");
        if (option_.on_enter) option_.on_enter();
        return true;
    }

    bool HandleCharacter(const std::string& character) {
        if (!option_.insert() && cursor() < content_->size() && (*content_)[cursor()] != '\n') {
            DeleteGlyph();
        }
        content_->insert(cursor(), character);
        set_cursor(cursor() + character.size());
        notify_change();
        return true;
    }

    bool DeleteGlyph() {
        if (cursor() >= content_->size()) return false;
        const size_t end = code_point_next(*content_, cursor());
        content_->erase(cursor(), end - cursor());
        return true;
    }

    bool HandleBackspace() {
        if (cursor() == 0) return false;
        const size_t start = code_point_previous(*content_, cursor());
        content_->erase(start, cursor() - start);
        set_cursor(start);
        notify_change();
        return true;
    }

    bool HandleDelete() {
        if (!DeleteGlyph()) return false;
        notify_change();
        return true;
    }

    bool HandleWordLeft() {
        size_t position = cursor();
        while (position > 0 && !is_word_byte((*content_)[code_point_previous(*content_, position)])) {
            position = code_point_previous(*content_, position);
        }
        while (position > 0 && is_word_byte((*content_)[code_point_previous(*content_, position)])) {
            position = code_point_previous(*content_, position);
        }
        return MoveTo(position);
    }

    bool HandleWordRight() {
        size_t position = cursor();
        while (position < content_->size() && is_word_byte((*content_)[position])) {
            position = code_point_next(*content_, position);
        }
        while (position < content_->size() && !is_word_byte((*content_)[position])) {
            position = code_point_next(*content_, position);
        }
        return MoveTo(position);
    }

    bool OnMouse(Event event) {
        hovered_ = box_.Contain(event.mouse().x, event.mouse().y) && CaptureMouse(event);
        if (!hovered_) return false;
        if (event.mouse().button != Mouse::Left || event.mouse().motion != Mouse::Pressed) return false;

        TakeFocus();
        if (content_->empty() || segments_.empty()) {
            set_cursor(0);
            return true;
        }

        // The cursor cell is the one thing whose screen position is known, so
        // the click is resolved as an offset from it -- that works no matter
        // how far the frame has scrolled the content.
        const int column = column_of(cursor()) + event.mouse().x - cursor_box_.x_min;
        const int line = static_cast<int>(cursor_segment_) + event.mouse().y - cursor_box_.y_min;
        const auto index = static_cast<size_t>(
            std::clamp(line, 0, static_cast<int>(segments_.size()) - 1));
        set_cursor(offset_at(segments_[index], std::max(0, column)));
        return true;
    }

    std::string* content_;
    std::string placeholder_;
    InputOption option_;

    std::vector<WrapSegment> segments_;
    size_t cursor_segment_ = 0;
    bool hovered_ = false;
    Box box_;
    Box inner_box_;
    Box cursor_box_;
};

}  // namespace

ftxui::Component WrappedInput(std::string* content, std::string placeholder, ftxui::InputOption option) {
    if (!option.multiline()) return ftxui::Input(content, std::move(placeholder), std::move(option));
    return ftxui::Make<WrappedInputBase>(content, std::move(placeholder), std::move(option));
}

}  // namespace callscribe::ui
