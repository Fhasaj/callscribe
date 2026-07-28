#include "util/subprocess.hpp"

#include <array>
#include <cstdio>

namespace callscribe::util {

std::string run_capture(const std::string& cmd) {
    std::array<char, 4096> buf{};
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), pipe)) > 0) {
        result.append(buf.data(), n);
    }
    pclose(pipe);
    return result;
}

}  // namespace callscribe::util
