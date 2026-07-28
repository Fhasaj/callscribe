#pragma once
#include <ftxui/component/component.hpp>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "context.hpp"

namespace callscribe::ui {

class SettingsScreen {
public:
    explicit SettingsScreen(AppContext& ctx);
    ~SettingsScreen();
    SettingsScreen(const SettingsScreen&) = delete;
    SettingsScreen& operator=(const SettingsScreen&) = delete;

    ftxui::Component component() { return root_; }

private:
    AppContext& ctx_;

    std::vector<std::string> ollama_models_;
    int ollama_model_selected_ = 0;
    std::atomic<bool> models_need_refresh_{false};

    std::vector<std::string> whisper_sizes_ = {"tiny", "base", "small", "medium", "large-v3"};
    int whisper_size_selected_ = 2;

    std::vector<std::string> mic_labels_;
    std::vector<std::string> mic_names_;
    int mic_selected_ = 0;
    std::vector<std::string> monitor_labels_;
    std::vector<std::string> monitor_names_;
    int monitor_selected_ = 0;

    std::string user_display_name_;
    std::string db_path_;
    std::string reminder_time_;
    bool reminder_enabled_ = true;

    std::mutex status_mutex_;
    std::string ollama_status_;
    std::string pull_status_;
    std::string whisper_status_;
    bool pulling_ = false;
    bool downloading_ = false;
    std::thread pull_thread_;
    std::thread download_thread_;

    ftxui::Component root_;

    void refresh_models();
    void refresh_audio_devices();
    void pull_recommended();
    void download_model();
    void save();
    void build_ui();
};

}  // namespace callscribe::ui
