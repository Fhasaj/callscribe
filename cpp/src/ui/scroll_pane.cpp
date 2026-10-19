#include "ui/scroll_pane.hpp"

#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <utility>

#include "ui/text_wrap.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

namespace callscribe::ui {

namespace {

using namespace ftxui;

class ScrollPaneBase : public ComponentBase {
public:
    ScrollPaneBase(std::function<Element(int width)> provider, std::function<int(int width)> line_count,
                    std::function<int()> follow_key = nullptr, std::string title = "")
        : provider_(std::move(provider)),
          line_count_(std::move(line_count)),
          follow_key_(std::move(follow_key)),
          title_(std::move(title)) {}

    Element OnRender() override {
        // box_ spans the whole pane, but vscroll_indicator keeps the rightmost
        // column for the scrollbar -- so the content gets one less than that.
        int width = std::max(1, box_.x_max - box_.x_min);
        view_lines_ = std::max(1, box_.y_max - box_.y_min + 1);

        // Content first: ScrollableText's line counter reads what the provider
        // just cached, so counting before fetching would size the scroll step
        // from the previous frame's text.
        Element inner = provider_(width);
        total_lines_ = line_count_ ? line_count_(width) : 0;

        // Jump to the bottom when new content arrives, but only then -- so a
        // user who has scrolled back stays where they put themselves.
        if (follow_key_) {
            int key = follow_key_();
            if (key != last_follow_key_) {
                last_follow_key_ = key;
                scroll_ = 1.f;
            }
        }
        Element pane = inner | focusPositionRelative(0.f, scroll_) | vscroll_indicator | yframe | flex |
                       reflect(box_);
        // Highlight the border while focused so it's clear which pane the
        // arrow keys will drive.
        const Color frame = Focused() ? theme::kAccent : theme::kMuted;
        if (!title_.empty())
            return titled_border(pane, text(" " + title_ + " ") | bold | color(theme::kAccent), frame);
        return pane | borderStyled(ROUNDED, frame);
    }

    bool Focusable() const override { return true; }

    bool OnEvent(Event event) override {
        if (event.is_mouse()) return OnMouse(event);
        if (!Focused()) return false;

        if (event == Event::ArrowUp) return Scroll(-line_step());
        if (event == Event::ArrowDown) return Scroll(line_step());
        if (event == Event::PageUp) return Scroll(-page_step());
        if (event == Event::PageDown) return Scroll(page_step());
        if (event == Event::Home) return Scroll(-1.f);
        if (event == Event::End) return Scroll(1.f);
        return false;
    }

private:
    // One line as a fraction of the scrollable range. When the content fits,
    // the frame clamps anyway, so a harmless default is fine.
    float line_step() const {
        int scrollable = total_lines_ - view_lines_;
        if (scrollable <= 0) return 0.05f;
        return 1.0f / static_cast<float>(scrollable);
    }

    float page_step() const { return line_step() * static_cast<float>(std::max(1, view_lines_ - 1)); }

    bool Scroll(float delta) {
        float next = std::clamp(scroll_ + delta, 0.f, 1.f);
        if (next == scroll_) return true;  // consumed, just already at the end
        scroll_ = next;
        return true;
    }

    bool OnMouse(Event event) {
        const auto& m = event.mouse();
        if (!box_.Contain(m.x, m.y)) return false;

        if (m.button == Mouse::WheelUp) return Scroll(-line_step() * 3.f);
        if (m.button == Mouse::WheelDown) return Scroll(line_step() * 3.f);

        // Clicking gives the pane focus so the keyboard bindings apply, but the
        // event must NOT be reported as handled: App::HandleSelection discards
        // the selection whenever a component claims the click, which would make
        // the text in here impossible to drag-select (and so impossible to copy).
        if (m.button == Mouse::Left && m.motion == Mouse::Pressed) TakeFocus();
        return false;
    }

    std::function<Element(int width)> provider_;
    std::function<int(int width)> line_count_;
    std::function<int()> follow_key_;
    std::string title_;
    int last_follow_key_ = -1;
    float scroll_ = 0.f;  // 0 = top, 1 = bottom
    Box box_;
    int total_lines_ = 0;
    int view_lines_ = 1;
};

class ScrollFormBase : public ComponentBase {
public:
    explicit ScrollFormBase(Component form) : form_(std::move(form)) { Add(form_); }

    Element OnRender() override {
        Element content = form_->Render();
        // Measured from the layout rather than with reflect(): reflect reports
        // only the part that is on screen, which is exactly the part that
        // doesn't tell us how far there is to scroll.
        content->ComputeRequirement();
        const Requirement& req = content->requirement();
        content_lines_ = req.min_y;

        const int ext = std::max(0, box_.y_max - box_.y_min);
        const int max_offset = std::max(0, content_lines_ - view_lines());
        if (follow_focus_) {
            // yframe centres whatever the focused component marked. Mirror its
            // arithmetic so that when the wheel takes over it starts from the
            // row actually on screen instead of jumping.
            const auto& focused = req.focused.box;
            offset_ = focused.y_min - ext / 2 + (focused.y_max - focused.y_min) / 2;
        } else {
            // focusPosition overrides the focused component's mark with a row
            // of our own, chosen so yframe's centring lands on offset_.
            content = content | focusPosition(0, std::clamp(offset_, 0, max_offset) + ext / 2);
        }
        offset_ = std::clamp(offset_, 0, max_offset);
        return content | vscroll_indicator | yframe | flex | reflect(box_);
    }

    bool OnEvent(Event event) override {
        if (event.is_mouse()) {
            const auto& m = event.mouse();
            if (box_.Contain(m.x, m.y) && (m.button == Mouse::WheelUp || m.button == Mouse::WheelDown)) {
                follow_focus_ = false;
                offset_ += m.button == Mouse::WheelUp ? -kWheelLines : kWheelLines;
                offset_ = std::clamp(offset_, 0, std::max(0, content_lines_ - view_lines()));
                return true;
            }
            return form_->OnEvent(event);
        }

        const bool handled = form_->OnEvent(event);
        // The ticker's redraws aren't the user moving anywhere.
        if (handled && event != Event::Custom) follow_focus_ = true;
        return handled;
    }

private:
    static constexpr int kWheelLines = 3;

    int view_lines() const { return std::max(1, box_.y_max - box_.y_min + 1); }

    Component form_;
    bool follow_focus_ = true;
    int offset_ = 0;  // first visible row
    int content_lines_ = 0;
    Box box_;
};

}  // namespace

ftxui::Component ScrollableText(std::function<std::string()> provider, std::string placeholder, std::string title) {
    // Wrap once per render and reuse for both the element and the line count,
    // so a mid-render change can't make them disagree.
    auto wrapped = std::make_shared<std::string>();
    auto element = [provider, placeholder, wrapped](int width) {
        std::string t = provider();
        *wrapped = WrapText(t.empty() ? placeholder : t, width);
        return ftxui::text(*wrapped);
    };
    auto lines = [wrapped](int) {
        return 1 + static_cast<int>(std::count(wrapped->begin(), wrapped->end(), '\n'));
    };
    return ftxui::Make<ScrollPaneBase>(element, lines, nullptr, std::move(title));
}

ftxui::Component ScrollableContent(std::function<ftxui::Element(int width)> provider,
                                    std::function<int()> follow_key) {
    return ftxui::Make<ScrollPaneBase>(std::move(provider), nullptr, std::move(follow_key));
}

ftxui::Component ScrollableForm(ftxui::Component form) { return ftxui::Make<ScrollFormBase>(std::move(form)); }

}  // namespace callscribe::ui
