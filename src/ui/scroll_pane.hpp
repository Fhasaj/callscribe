#pragma once
#include <ftxui/component/component.hpp>

#include <functional>
#include <string>

namespace callscribe::ui {

// A read-only, scrollable text pane for content that outgrows its box
// (transcripts, summaries, generated reports).
//
// Scrolls with the mouse wheel while the pointer is over it, and with
// Up/Down/PageUp/PageDown/Home/End once it has focus (clicking it takes
// focus). The pane draws its own rounded border and a scrollbar on the right,
// so callers replace the whole `frame(paragraph(x)) | border` expression with
// `pane->Render()`.
//
// The text is word-wrapped to the pane's width and rendered as real lines (see
// ui/text_wrap.hpp), so drag-selecting it copies the spaces and line breaks
// that are on screen.
//
// `provider` is called on every render, so screens keep owning their text (and
// their mutex) -- take the lock inside the callback.
ftxui::Component ScrollableText(std::function<std::string()> provider, std::string placeholder = "");

// Same behaviour for content that isn't a single string (e.g. a stack of
// message bubbles). Line-accurate stepping isn't possible here, so it scrolls
// by a small fraction of the content per notch. `provider` is handed the pane's
// width in columns so it can wrap its own text with WrappedText().
//
// `follow_key` makes the pane chat-like: whenever the key changes (pass e.g. a
// message count) it jumps to the bottom, so new content is shown, while leaving
// manual scrolling untouched the rest of the time.
ftxui::Component ScrollableContent(std::function<ftxui::Element(int width)> provider,
                                    std::function<int()> follow_key = nullptr);

}  // namespace callscribe::ui
