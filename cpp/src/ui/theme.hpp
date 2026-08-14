#pragma once
#include <ftxui/screen/color.hpp>

// Shared color palette for the TUI -- named Palette16 colors (not RGB
// truecolor) so it renders correctly across terminals/tmux/ssh sessions
// without a truecolor-capable terminfo entry.
namespace callscribe::ui::theme {

inline const ftxui::Color kAccent = ftxui::Color::Cyan;      // headers, active tab, primary actions
inline const ftxui::Color kSuccess = ftxui::Color::Green;    // done / healthy status
inline const ftxui::Color kWarning = ftxui::Color::Yellow;   // due today, in-progress work
inline const ftxui::Color kDanger = ftxui::Color::Red;       // overdue, errors, recording
inline const ftxui::Color kMuted = ftxui::Color::GrayDark;   // secondary text, panel borders

}  // namespace callscribe::ui::theme
