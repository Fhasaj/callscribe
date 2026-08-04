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

// Runs `prog` with stdout captured. `ok` reports whether it exited cleanly;
// callers use it to fall through to the next clipboard tool rather than
// treating a genuinely empty clipboard as a failure.
std::string run_capture_stdout(const char* prog, const std::vector<std::string>& args, bool& ok) {
    ok = false;
    int fds[2];
    if (pipe(fds) != 0) return "";
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return "";
    }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
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
    close(fds[1]);
    std::string out;
    char buf[4096];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof(buf))) > 0) out.append(buf, static_cast<size_t>(n));
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    return out;
}

}  // namespace

bool copy_to_clipboard(const std::string& text) {
    if (command_exists("wl-copy")) return run_with_stdin("wl-copy", {}, text);
    if (command_exists("xclip")) return run_with_stdin("xclip", {"-selection", "clipboard"}, text);
    if (command_exists("xsel")) return run_with_stdin("xsel", {"--clipboard", "--input"}, text);
    return false;
}

std::string paste_from_clipboard() {
    bool ok = false;
    // -n: wl-paste otherwise appends a trailing newline that isn't in the
    // clipboard, which would show up in every pasted field.
    if (command_exists("wl-paste")) {
        std::string out = run_capture_stdout("wl-paste", {"-n"}, ok);
        if (ok) return out;
    }
    if (command_exists("xclip")) {
        std::string out = run_capture_stdout("xclip", {"-selection", "clipboard", "-o"}, ok);
        if (ok) return out;
    }
    if (command_exists("xsel")) {
        std::string out = run_capture_stdout("xsel", {"--clipboard", "--output"}, ok);
        if (ok) return out;
    }
    return "";
}

}  // namespace callscribe::util
