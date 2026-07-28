#include "util/clipboard.hpp"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <vector>

namespace callscribe::util {

namespace {

bool command_exists(const char* name) {
    std::string cmd = std::string("command -v ") + name + " >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}

bool run_with_stdin(const char* prog, const std::vector<std::string>& args, const std::string& input) {
    int fds[2];
    if (pipe(fds) != 0) return false;
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    if (pid == 0) {
        close(fds[1]);
        dup2(fds[0], STDIN_FILENO);
        close(fds[0]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(prog));
        for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(prog, argv.data());
        _exit(127);
    }
    close(fds[0]);
    ssize_t written = 0;
    while (written < static_cast<ssize_t>(input.size())) {
        ssize_t n = write(fds[1], input.data() + written, input.size() - written);
        if (n <= 0) break;
        written += n;
    }
    close(fds[1]);
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

}  // namespace

bool copy_to_clipboard(const std::string& text) {
    if (command_exists("wl-copy")) return run_with_stdin("wl-copy", {}, text);
    if (command_exists("xclip")) return run_with_stdin("xclip", {"-selection", "clipboard"}, text);
    if (command_exists("xsel")) return run_with_stdin("xsel", {"--clipboard", "--input"}, text);
    return false;
}

}  // namespace callscribe::util
