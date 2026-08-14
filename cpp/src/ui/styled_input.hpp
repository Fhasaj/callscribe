#pragma once
#include <ftxui/component/component_options.hpp>
#include <ftxui/dom/elements.hpp>

#include "ui/theme.hpp"

namespace callscribe::ui {

// A boxed InputOption: rounded border (accent when focused, muted otherwise)
// instead of ftxui's plain inline text field, so every text box in the app
// reads as a distinct control. Set multiline/on_enter/on_change on the
// returned option same as any other InputOption.
inline ftxui::InputOption styled_input_option() {
    ftxui::InputOption opt;
    opt.transform = [](ftxui::InputState state) {
        using namespace ftxui;
        if (state.is_placeholder) state.element |= dim;
        if (state.focused) state.element |= inverted;
        return state.element | borderStyled(ROUNDED, state.focused ? theme::kAccent : theme::kMuted);
    };
    return opt;
}

}  // namespace callscribe::ui
