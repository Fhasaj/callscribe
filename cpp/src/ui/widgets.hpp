#pragma once
#include <ftxui/component/component_options.hpp>
#include <ftxui/dom/elements.hpp>

#include <string>

#include "ui/theme.hpp"

// The few building blocks every screen is laid out from, so they all look like
// one app: one button style, titled sections, and label/field rows that line
// up. Buttons and styled inputs are both three rows tall (a rounded border
// around one line), which is what lets a row of them sit level.
namespace callscribe::ui {

// An outlined button in `tint`, filled solid while focused so it's obvious
// where Enter will land. kMuted is for secondary actions: grey outline,
// normal-coloured label.
inline ftxui::ButtonOption button_style(ftxui::Color tint = theme::kAccent) {
    ftxui::ButtonOption opt;
    opt.transform = [tint](const ftxui::EntryState& s) {
        using namespace ftxui;
        const bool secondary = tint == theme::kMuted;
        Element label = text(" " + s.label + " ");
        if (s.focused) {
            label = label | bold | color(Color::Black) | bgcolor(secondary ? Color::GrayLight : tint);
            return label | borderStyled(ROUNDED, secondary ? Color::GrayLight : tint);
        }
        if (!secondary) label = label | bold | color(tint);
        return label | borderStyled(ROUNDED, secondary ? theme::kMuted : tint);
    };
    return opt;
}

// A one-row text link for buttons inside lists ("[edit]", "[done]"), where a
// three-row button would be far too heavy.
inline ftxui::ButtonOption link_style(ftxui::Color tint = theme::kAccent) {
    ftxui::ButtonOption opt;
    opt.transform = [tint](const ftxui::EntryState& s) {
        using namespace ftxui;
        Element e = text(s.label) | color(tint);
        if (s.focused) e |= inverted;
        return e;
    };
    return opt;
}

// A rounded border in `border_color` with `title` set into its top edge.
// Built by hand rather than with window(): window() takes its border colour
// from the inherited foreground, and there's no way to hand the content its
// own default colour back -- the content came out grey along with the frame.
inline ftxui::Element titled_border(ftxui::Element content, ftxui::Element title, ftxui::Color border_color) {
    using namespace ftxui;
    return dbox({
        content | borderStyled(ROUNDED, border_color),
        // Skips the corner cell, then writes the title over the top edge. The
        // filler below keeps it to that one row: stretched to the full
        // height, its colour and bold would paint a column down the panel.
        vbox({hbox({emptyElement() | size(WIDTH, EQUAL, 1), title, filler()}), filler()}),
    });
}

// A titled, rounded panel: the unit every screen is grouped into.
inline ftxui::Element section(const std::string& title, ftxui::Element content,
                              ftxui::Color title_color = theme::kAccent) {
    using namespace ftxui;
    return titled_border(std::move(content), text(" " + title + " ") | bold | color(title_color), theme::kMuted);
}

// A label in a fixed-width column beside a (usually three-row) field, centred
// on it vertically so the pair reads as one row.
inline ftxui::Element field_row(const std::string& label, ftxui::Element field, int label_width = 16) {
    using namespace ftxui;
    return hbox({text(label) | color(Color::GrayLight) | size(WIDTH, EQUAL, label_width) | vcenter, field | flex});
}

// Short coloured status pill: " ● Active ".
inline ftxui::Element badge(const std::string& label, ftxui::Color tint) {
    using namespace ftxui;
    return hbox({text(" ● ") | color(tint), text(label + " ") | bold | color(tint)});
}

}  // namespace callscribe::ui
