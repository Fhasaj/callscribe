#include "ui/app.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <atomic>
#include <chrono>
#include <thread>

#include "ui/history_screen.hpp"
#include "ui/proofread_screen.hpp"
#include "ui/record_screen.hpp"
#include "ui/settings_screen.hpp"
#include "ui/summary_screen.hpp"
#include "ui/tasks_screen.hpp"

namespace callscribe::ui {

int run(AppContext& ctx) {
    using namespace ftxui;

    auto screen = ScreenInteractive::Fullscreen();

    RecordScreen record(ctx);
    HistoryScreen history(ctx);
    TasksScreen tasks(ctx);
    SummaryScreen summary(ctx);
    ProofreadScreen proofread(ctx);
    SettingsScreen settings(ctx);

    std::vector<std::string> tab_titles = {"Record", "History", "Tasks", "Yearly Summary", "Proofread", "Settings"};
    int tab_index = 0;
    auto tab_menu = Menu(&tab_titles, &tab_index, MenuOption::HorizontalAnimated());

    auto tab_container = Container::Tab(
        {
            record.component(),
            history.component(),
            tasks.component(),
            summary.component(),
            proofread.component(),
            settings.component(),
        },
        &tab_index);

    auto main_container = Container::Vertical({tab_menu, tab_container});

    auto main_renderer = Renderer(main_container, [&] {
        return vbox({
                   text("CallScribe") | bold | hcenter,
                   tab_menu->Render(),
                   separator(),
                   tab_container->Render() | flex,
               }) |
               flex;
    });

    std::atomic<bool> ticking{true};
    std::thread ticker([&] {
        while (ticking) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (ticking) screen.PostEvent(Event::Custom);
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
