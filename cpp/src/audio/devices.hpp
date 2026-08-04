#pragma once
#include <string>
#include <vector>

namespace callscribe::audio {

struct AudioSource {
    std::string name;
    std::string description;
    bool is_monitor = false;
};

std::vector<AudioSource> list_sources();
std::vector<AudioSource> list_mic_sources();
std::vector<AudioSource> list_monitor_sources();
std::string default_mic_source();
std::string default_monitor_source();
bool audio_backend_available();

}  // namespace callscribe::audio
