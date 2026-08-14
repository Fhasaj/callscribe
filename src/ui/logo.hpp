#pragma once
#include <ftxui/dom/elements.hpp>

#include <string>
#include <vector>

#include "ui/theme.hpp"

namespace callscribe::ui {

// Block-capital wordmark: 5 rows, 49 columns. Solid blocks rather than the
// ╔══╗ box-drawing style -- blocks are in every terminal font, the double-line
// characters are not, and a wordmark that renders as tofu is worse than none.
inline const std::vector<std::string>& logo_rows() {
    static const std::vector<std::string> rows = {
        " ███  ██  █    █     ███  ███ ███  ███  ███  ████",
        "█    █  █ █    █    █    █    █  █  █   █  █ █",
        "█    ████ █    █     ██  █    ███   █   ███  ███",
        "█    █  █ █    █       █ █    █ █   █   █  █ █",
        " ███ █  █ ████ ████ ███   ███ █  █ ███  ███  ████",
    };
    return rows;
}

inline constexpr int kLogoWidth = 49;

// The same wordmark at half the height, for the title bar. Each line packs two
// rows of logo_rows() into one cell using the half-block characters: ▀ is a
// pixel in the upper row only, ▄ the lower only, █ both. Kept as literals
// rather than folded from logo_rows() at runtime -- the block characters are
// multi-byte UTF-8, so pairing them up would mean decoding first.
inline const std::vector<std::string>& logo_rows_mini() {
    static const std::vector<std::string> rows = {
        "▄▀▀▀ ▄▀▀▄ █    █    ▄▀▀▀ ▄▀▀▀ █▀▀▄ ▀█▀  █▀▀▄ █▀▀▀",
        "█    █▀▀█ █    █     ▀▀▄ █    █▀█   █   █▀▀▄ █▀▀",
        " ▀▀▀ ▀  ▀ ▀▀▀▀ ▀▀▀▀ ▀▀▀   ▀▀▀ ▀  ▀ ▀▀▀  ▀▀▀  ▀▀▀▀",
    };
    return rows;
}

inline constexpr int kLogoMiniWidth = 49;
inline constexpr int kLogoMiniHeight = 3;

// Title-bar wordmark. Returns an empty optional-ish marker by way of a plain
// text() when there isn't room -- callers check kLogoMiniWidth themselves when
// they need to know whether to reserve the extra rows.
inline ftxui::Element LogoMini() {
    using namespace ftxui;
    Elements lines;
    int row = 0;
    for (const auto& r : logo_rows_mini()) {
        Element e = text(r) | color(theme::kAccent);
        if (row == 0) e |= bold;
        if (row == 2) e |= dim;
        lines.push_back(e);
        ++row;
    }
    return vbox(std::move(lines));
}

// The wordmark, shaded top-to-bottom so it reads as a mark rather than a wall
// of text. Falls back to a plain one-line title below kLogoWidth columns.
inline ftxui::Element LogoMark(int available_width) {
    using namespace ftxui;
    if (available_width < kLogoWidth) {
        return text("C A L L S C R I B E") | bold | color(theme::kAccent);
    }
    Elements lines;
    int row = 0;
    for (const auto& r : logo_rows()) {
        Element e = text(r) | color(theme::kAccent);
        if (row < 3) e |= bold;      // brighter at the top,
        if (row >= 3) e |= dim;      // fading toward the baseline
        lines.push_back(e);
        ++row;
    }
    return vbox(std::move(lines));
}

// Wordmark plus tagline, centered -- the startup splash and the Chat screen's
// empty state.
inline ftxui::Element LogoBanner(int available_width, const std::string& tagline) {
    using namespace ftxui;
    Elements lines;
    lines.push_back(LogoMark(available_width) | center);
    if (!tagline.empty()) {
        lines.push_back(text("") );
        lines.push_back(text(tagline) | dim | center);
    }
    return vbox(std::move(lines));
}

}  // namespace callscribe::ui
