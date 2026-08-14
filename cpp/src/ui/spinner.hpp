#pragma once
#include <ftxui/dom/elements.hpp>

#include <chrono>
#include <string>
#include <vector>

#include "ui/theme.hpp"

namespace callscribe::ui {

// A live "the model is working" line, in the shape Claude Code uses: an
// animated glyph, a verb that changes every few seconds so a long wait doesn't
// look frozen, the elapsed time, and whatever detail the caller has (streamed
// character counts, the current phase).
//
// Nothing here schedules a repaint -- app.cpp already ticks the screen every
// 80ms, which is what makes the animation move.

namespace detail {

// Braille dots: one cell, no width surprises, and present in every font that
// covers the block characters the logo already uses.
inline const std::vector<std::string>& spinner_frames() {
    static const std::vector<std::string> frames = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
    return frames;
}

inline const std::vector<std::string>& waiting_words() {
    static const std::vector<std::string> words = {"Thinking", "Pondering", "Mulling",  "Chewing on it",
                                                    "Working",  "Reasoning", "Composing"};
    return words;
}

inline long long elapsed_ms(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - since).count();
}

}  // namespace detail

inline const std::string& SpinnerFrame(std::chrono::steady_clock::time_point since) {
    const auto& frames = detail::spinner_frames();
    const auto index = static_cast<size_t>(detail::elapsed_ms(since) / 100) % frames.size();
    return frames[index];
}

// `verb` empty means "rotate through the waiting words"; pass a fixed string
// (e.g. "Proofreading") to keep one word for the whole wait.
inline ftxui::Element ThinkingIndicator(std::chrono::steady_clock::time_point since, const std::string& verb = "",
                                         const std::string& detail_text = "") {
    using namespace ftxui;
    const long long ms = detail::elapsed_ms(since);

    std::string word = verb;
    if (word.empty()) {
        const auto& words = detail::waiting_words();
        word = words[static_cast<size_t>(ms / 4000) % words.size()];
    }

    std::string meta = "(" + std::to_string(ms / 1000) + "s";
    if (!detail_text.empty()) meta += " · " + detail_text;
    meta += ")";

    return hbox({
        text(SpinnerFrame(since) + " ") | color(theme::kAccent) | bold,
        text(word + "… ") | color(theme::kAccent),
        text(meta) | dim,
    });
}

}  // namespace callscribe::ui
