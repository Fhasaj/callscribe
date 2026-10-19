#include "ui/app.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

#include "ui/calendar_screen.hpp"
#include "ui/chat_screen.hpp"
#include "ui/focus_screen.hpp"
#include "ui/history_screen.hpp"
#include "ui/logo.hpp"
#include "ui/mouse_mover_screen.hpp"
#include "ui/quick_capture.hpp"
#include "ui/record_screen.hpp"
#include "ui/settings_screen.hpp"
#include "ui/summary_screen.hpp"
#include "ui/tasks_screen.hpp"
#include "ui/text_wrap.hpp"
#include "ui/today_screen.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"
#include "util/clipboard.hpp"
#include "util/time.hpp"

namespace callscribe::ui {

namespace {
constexpr int kTasksTabIndex = 2;
constexpr int kMouseMoverTabIndex = 6;
constexpr int kFocusTabIndex = 7;

// Not a real terminal event -- posted to make FTXUI drop the current
// selection. Any event the component tree reports as handled clears it (see
// App::Internal::HandleSelection), so this one exists purely to be swallowed.
const ftxui::Event kDeselectEvent = ftxui::Event::Special("callscribe-deselect");
}  // namespace

int run(AppContext& ctx) {
    using namespace ftxui;

    auto screen = ScreenInteractive::Fullscreen();

    RecordScreen record(ctx);
    HistoryScreen history(ctx);
    TasksScreen tasks(ctx);
    // Not named `calendar` -- that would shadow the callscribe::calendar
    // namespace inside this one.
    CalendarScreen calendar_screen(ctx);
    SummaryScreen summary(ctx);
    ChatScreen chat(ctx);
    MouseMoverScreen mouse_mover(ctx);
    // A call being recorded counts as "still going" for the hyperfocus check-in.
    FocusScreen focus_screen(ctx, screen, [&record] { return record.is_recording(); });
    TodayScreen today(ctx);
    SettingsScreen settings(ctx);

    std::vector<std::string> tab_titles = {"Record", "History",     "Tasks", "Calendar", "Review",
                                            "Chat",   "Mouse Mover", "Focus", "Today",    "Settings"};
    int tab_index = 0;

    // "record" on a calendar meeting fills the Record form in and brings that
    // tab forward -- the point of the button is not having to retype what the
    // invite already says.
    calendar_screen.set_on_record([&record, &tab_index](const std::string& meeting_name,
                                                          const std::vector<std::string>& participants) {
        record.prefill(meeting_name, "", participants);
        tab_index = 0;
    });

    // Reminder that's visible no matter which tab is active: recomputed once a
    // second (below) from open tasks due today or earlier, and rendered as a
    // colored badge on the Tasks entry -- the daily notify-send popup is easy
    // to miss, this isn't.
    std::atomic<int> tasks_alert_count{0};

    MenuOption tab_opt = MenuOption::HorizontalAnimated();
    tab_opt.entries_option.transform = [&tasks_alert_count, &mouse_mover, &focus_screen](const EntryState& state) {
        std::string label = " " + state.label;
        int alerts = (state.index == kTasksTabIndex) ? tasks_alert_count.load() : 0;
        if (alerts > 0) label += " (" + std::to_string(alerts) + ")";
        // The mouse mover runs in the background across every tab, so its
        // on/off state has to be visible from every tab too.
        bool jiggling = state.index == kMouseMoverTabIndex && mouse_mover.is_running();
        if (jiggling) label += " *";
        // Same idea for a running focus timer: the countdown follows you.
        FocusScreen::Badge focus_badge =
            state.index == kFocusTabIndex ? focus_screen.tab_badge() : FocusScreen::Badge{"", theme::kMuted};
        label += focus_badge.text;

        label += " ";
        Element e = text(label);
        if (state.active) e |= bold | color(theme::kAccent);
        if (alerts > 0 && !state.active) e |= color(theme::kDanger);
        if (jiggling && !state.active) e |= color(theme::kSuccess);
        if (!focus_badge.text.empty() && !state.active) e |= color(focus_badge.color);
        if (state.focused) e |= inverted;
        return e;
    };
    auto tab_menu = Menu(&tab_titles, &tab_index, tab_opt);

    auto tab_container = Container::Tab(
        {
            record.component(),
            history.component(),
            tasks.component(),
            calendar_screen.component(),
            summary.component(),
            chat.component(),
            mouse_mover.component(),
            focus_screen.component(),
            today.component(),
            settings.component(),
        },
        &tab_index);

    // Window chrome: a close button top-right, guarded by a confirmation
    // popup if a recording is in flight (quitting still finalizes it --
    // RecordScreen's destructor blocks on that -- but transcribing +
    // summarizing can take a while, so warn before the terminal seems to hang).
    bool close_confirm_visible = false;
    // A plain (non-Animated) style -- ButtonOption::Animated wraps its label
    // in borderEmpty padding, which would make this 3 lines tall and blow up
    // the single-line title bar it sits in.
    ButtonOption close_button_opt;
    close_button_opt.transform = [](const EntryState& s) {
        Element e = text(s.label) | bold | color(theme::kDanger);
        if (s.focused) e |= inverted;
        return e;
    };
    auto close_button = Button(
        " x ",
        [&] {
            if (record.is_recording())
                close_confirm_visible = true;
            else
                screen.Exit();
        },
        close_button_opt);

    auto confirm_quit_btn = Button(
        "Quit anyway",
        [&] {
            close_confirm_visible = false;
            screen.Exit();
        },
        button_style(theme::kDanger));
    auto cancel_quit_btn = Button(
        "Keep recording", [&] { close_confirm_visible = false; }, button_style(theme::kAccent));
    auto confirm_quit_buttons = Container::Horizontal({confirm_quit_btn, cancel_quit_btn});
    auto quit_confirm_modal = Renderer(confirm_quit_buttons, [confirm_quit_btn, cancel_quit_btn] {
        return vbox({
                   text("Recording in progress") | bold | color(theme::kDanger),
                   text("Quitting will stop and process this call (transcribe +"),
                   text("summarize) before exiting -- that can take a moment."),
                   separator() | color(theme::kMuted),
                   hbox({confirm_quit_btn->Render(), text("  "), cancel_quit_btn->Render()}) | hcenter,
               }) |
               size(WIDTH, EQUAL, 54) | borderStyled(ROUNDED, theme::kDanger) | bgcolor(Color::Black) | clear_under |
               center;
    });

    // Right-click context menu ------------------------------------------
    // Terminal apps swallow the mouse, so the terminal's own copy/paste is
    // unavailable while we're in mouse-tracking mode. This gives back both:
    // Copy takes whatever was mouse-dragged (screen.GetSelection()), Paste
    // feeds the system clipboard to the focused Input as a Character event.
    bool context_visible = false;
    int context_x = 0;
    int context_y = 0;
    // The selection has to be snapshotted at right-click time: clicking an
    // entry is a left-click, and FTXUI drops the selection on the next click.
    std::string context_selection;
    std::string context_note;
    auto context_note_at = std::chrono::steady_clock::now();

    constexpr int kContextWidth = 20;   // including the border columns
    constexpr int kContextHeight = 7;   // 4 entries + separator + border rows

    auto set_note = [&](std::string note) {
        context_note = std::move(note);
        context_note_at = std::chrono::steady_clock::now();
    };

    ButtonOption context_entry_opt;
    context_entry_opt.transform = [](const EntryState& s) {
        Element e = text(" " + s.label);
        if (s.focused) e |= inverted;
        return e | flex;
    };

    // The selection comes off the screen, so it carries the line breaks the
    // panes wrapped the text with. UnwrapSelection puts the paragraphs back
    // together, leaving the author's own line breaks in place.
    auto do_copy = [&](const std::string& selection) {
        if (selection.empty()) {
            set_note("Nothing selected -- drag over text first");
        } else if (util::copy_to_clipboard(UnwrapSelection(selection))) {
            set_note("Copied to clipboard");
        } else {
            set_note("No clipboard tool (install wl-clipboard or xclip)");
        }
    };

    auto do_paste = [&] {
        std::string text_in = util::paste_from_clipboard();
        if (text_in.empty()) {
            set_note("Clipboard is empty");
            return;
        }
        // Posted rather than dispatched inline so it lands after the menu has
        // closed and focus is back on the underlying input. It goes to whatever
        // Input currently has focus -- click into the field first.
        screen.PostEvent(Event::Character(text_in));
        set_note("Pasted into the focused field");
    };

    // FTXUI has no API to set the selection directly -- it is built purely from
    // drag events -- so "Select all" replays the drag it would have taken:
    // press outside the top-left corner, move past the bottom-right, release.
    // The out-of-bounds coordinates matter twice over: no component's box
    // contains them (a component that handled the press would cancel the
    // selection instead of starting one), and the range then covers every cell
    // regardless of how the screen is laid out.
    auto do_select_all = [&] {
        Mouse m;
        m.button = Mouse::Left;
        m.motion = Mouse::Pressed;
        m.x = -1;
        m.y = -1;
        screen.PostEvent(Event::Mouse("", m));

        m.motion = Mouse::Moved;
        m.x = screen.dimx() + 1;
        m.y = screen.dimy() + 1;
        screen.PostEvent(Event::Mouse("", m));

        m.motion = Mouse::Released;
        screen.PostEvent(Event::Mouse("", m));

        set_note("Selected everything -- ^G to copy");
    };

    // ^W / ^P from any tab: catch a win or a stray thought without losing
    // your place.
    QuickCapture capture(
        ctx, [&focus_screen] { return focus_screen.current_tag(); }, [&](const std::string& m) { set_note(m); });

    auto do_deselect_all = [&] {
        screen.PostEvent(kDeselectEvent);
        context_selection.clear();
        set_note("Selection cleared");
    };

    auto copy_entry = Button(
        "Copy",
        [&] {
            context_visible = false;
            do_copy(context_selection);
        },
        context_entry_opt);

    auto paste_entry = Button(
        "Paste",
        [&] {
            context_visible = false;
            do_paste();
        },
        context_entry_opt);

    auto select_all_entry = Button(
        "Select all",
        [&] {
            // Closed first so the menu itself is gone by the time the synthetic
            // drag arrives -- otherwise it would be part of the selection, and
            // its own click handler would eat the press.
            context_visible = false;
            do_select_all();
        },
        context_entry_opt);

    auto deselect_all_entry = Button(
        "Deselect all",
        [&] {
            context_visible = false;
            do_deselect_all();
        },
        context_entry_opt);

    auto context_container =
        Container::Vertical({copy_entry, paste_entry, select_all_entry, deselect_all_entry});

    auto main_container = Container::Vertical({tab_menu, tab_container, close_button});

    // The context menu is a sibling of the main UI in the focus chain rather
    // than a Modal: Modal applies `clear_under` to the whole modal element, and
    // ours spans the screen (the spacers that pin it to the cursor), which would
    // blank the entire UI behind it.
    int root_selector = 0;
    auto root_container = Container::Tab({main_container, context_container}, &root_selector);

    // Startup splash: the wordmark on its own for a couple of seconds, or until
    // the first keypress/click. Whisper is loading in the background at this
    // point anyway, so the pause costs nothing.
    bool splash_visible = true;
    const auto splash_started = std::chrono::steady_clock::now();
    constexpr auto kSplashDuration = std::chrono::milliseconds(2200);

    auto main_renderer = Renderer(root_container, [&] {
        if (splash_visible && std::chrono::steady_clock::now() - splash_started > kSplashDuration)
            splash_visible = false;
        if (splash_visible) {
            return vbox({
                       filler(),
                       LogoBanner(screen.dimx(), "Local call notes, summaries and tasks -- nothing leaves this machine."),
                       text("") ,
                       text("press any key") | dim | center,
                       filler(),
                   }) |
                   flex | borderStyled(ROUNDED, theme::kMuted);
        }

        Element status = text("  ^G copy  ^V paste  ^W win  ^P park  ") | color(theme::kMuted);
        // What you're meant to be doing, from every tab -- the one line that
        // gets you back on track after wandering into History.
        if (FocusScreen::Badge note = focus_screen.title_note(); !note.text.empty())
            status = text("  " + note.text + "  ") | color(note.color) | bold;
        if (!context_note.empty()) {
            if (std::chrono::steady_clock::now() - context_note_at > std::chrono::seconds(3))
                context_note.clear();
            else
                status = text("  " + context_note + "  ") | color(theme::kAccent);
        }
        // The wordmark is three rows tall, so it only goes in the title bar
        // when there is room for it *and* the status hints beside it; below
        // that the bar falls back to one row of plain text.
        constexpr int kTitleStatusWidth = 46;  // hints + close button, roughly
        const bool room_for_logo = screen.dimx() >= kLogoMiniWidth + kTitleStatusWidth;
        Element titlebar =
            room_for_logo ? hbox({
                                text(" "),
                                LogoMini(),
                                filler(),
                                // Pinned to the top row of the three, where the
                                // close button was before the bar grew.
                                vbox({hbox({status, close_button->Render()}), filler()}),
                            })
                          : hbox({
                                text(" CallScribe ") | bold | color(theme::kAccent),
                                filler(),
                                status,
                                close_button->Render(),
                            });
        Element document = vbox({
                               titlebar,
                               separator() | color(theme::kMuted),
                               // The animated menu draws its own rule underneath.
                               tab_menu->Render(),
                               tab_container->Render() | flex,
                           }) |
                           flex | borderStyled(ROUNDED, theme::kMuted);

        if (context_visible) {
            // clear_under goes on the small box only, so the UI stays visible
            // behind it; the spacers pin its top-left corner to the cursor.
            Element box = vbox({
                              copy_entry->Render(),
                              paste_entry->Render(),
                              separator() | color(theme::kMuted),
                              select_all_entry->Render(),
                              deselect_all_entry->Render(),
                          }) |
                          size(WIDTH, EQUAL, kContextWidth - 2) | borderStyled(ROUNDED, theme::kAccent) |
                          bgcolor(Color::Black) | clear_under;
            Element overlay = vbox({
                emptyElement() | size(HEIGHT, EQUAL, context_y),
                hbox({emptyElement() | size(WIDTH, EQUAL, context_x), box, filler()}),
                filler(),
            });
            document = dbox({document, overlay});
        }
        return document;
    });
    main_renderer |= Modal(quit_confirm_modal, &close_confirm_visible);
    main_renderer |= Modal(capture.component(), capture.visible());

    main_renderer |= CatchEvent([&](Event event) {
        // Any real input dismisses the splash and is swallowed, so the keypress
        // that skips it doesn't also land on whatever it was covering. The
        // ticker's Event::Custom is not real input -- it would close the splash
        // on the first frame.
        if (splash_visible && (event.is_character() || event.is_mouse() || event == Event::Return ||
                               event == Event::Escape)) {
            splash_visible = false;
            return true;
        }

        // Keep the focus chain in step with the menu on every event, not just
        // on render, so clicks on the menu reach its buttons on the same frame.
        root_selector = context_visible ? 1 : 0;

        // Keyboard equivalents. Terminals differ on whether they forward
        // right-clicks to the application (many pop their own menu instead),
        // so these are the reliable path.
        // Ctrl+G rather than Ctrl+B: Ctrl+B is tmux's prefix and never reaches
        // the app there.
        if (event == Event::CtrlG) {
            do_copy(screen.GetSelection());
            return true;
        }
        if (event == Event::CtrlV) {
            do_paste();
            return true;
        }
        const bool overlay_open = context_visible || close_confirm_visible || *capture.visible();
        if (!overlay_open && (event == Event::CtrlW || event == Event::CtrlP)) {
            capture.open(event == Event::CtrlW ? QuickCapture::Kind::Win : QuickCapture::Kind::Park);
            return true;
        }
        // Taken here, ahead of the tab menu, so they work wherever focus is
        // within the Focus tab -- including on the menu itself.
        if (tab_index == kFocusTabIndex && !overlay_open && focus_screen.handle_shortcut(event)) return true;
        // Returning true is the whole point here: FTXUI clears the selection
        // for any event the tree reports as handled.
        if (event == kDeselectEvent) return true;

        if (context_visible && event == Event::Escape) {
            context_visible = false;
            return true;
        }
        if (!event.is_mouse()) return false;
        const auto& m = event.mouse();

        if (m.button == Mouse::Right && m.motion == Mouse::Pressed) {
            context_selection = screen.GetSelection();
            // Clamp so the menu stays fully on screen when right-clicking near
            // the bottom/right edge.
            context_x = std::max(0, std::min(m.x - 1, screen.dimx() - kContextWidth));
            context_y = std::max(0, std::min(m.y - 1, screen.dimy() - kContextHeight));
            context_visible = true;
            return true;
        }

        // Any click outside the open menu dismisses it, and is swallowed so it
        // doesn't also activate whatever is underneath.
        if (context_visible && m.motion == Mouse::Pressed) {
            bool inside = m.x - 1 >= context_x && m.x - 1 < context_x + kContextWidth && m.y - 1 >= context_y &&
                          m.y - 1 < context_y + kContextHeight;
            if (!inside) {
                context_visible = false;
                return true;
            }
        }
        return false;
    });

    // 80ms redraw tick -- fast enough that the recording/test level gauges
    // (updated ~10x/sec by StreamCapture) read as live rather than jumpy.
    //
    // Nothing on screen moves on its own the rest of the time, so idling at
    // that rate meant re-rendering the whole component tree 12.5x/second
    // forever. The slow tick still refreshes anything a background thread
    // changed (a finished summary, a fired reminder) without the constant
    // wakeups; screens with live output opt back into the fast one.
    constexpr auto kFastTick = std::chrono::milliseconds(80);
    constexpr auto kIdleTick = std::chrono::milliseconds(500);

    std::atomic<bool> ticking{true};
    std::thread ticker([&] {
        auto elapsed_since_task_check = std::chrono::milliseconds(0);
        while (ticking) {
            const bool live = record.is_recording() || settings.needs_fast_redraw() || chat.needs_fast_redraw();
            const auto interval = live ? kFastTick : kIdleTick;
            std::this_thread::sleep_for(interval);
            if (!ticking) break;
            elapsed_since_task_check += interval;
            if (elapsed_since_task_check >= std::chrono::seconds(1)) {
                elapsed_since_task_check = std::chrono::milliseconds(0);
                std::string today = util::today_iso();
                int count = 0;
                for (auto& t : ctx.repo->list_tasks("open")) {
                    if (!t.due_date.empty() && t.due_date <= today) ++count;
                }
                tasks_alert_count.store(count);
            }
            screen.PostEvent(Event::Custom);
        }
    });

    ctx.preload_whisper();
    ctx.reminder->set_on_fired([&](const std::vector<db::Task>&) { screen.PostEvent(Event::Custom); });
    ctx.reminder->start();

    screen.Loop(main_renderer);

    ticking = false;
    ticker.join();
    ctx.reminder->stop();
    return 0;
}

}  // namespace callscribe::ui
