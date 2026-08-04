#pragma once
#include "context.hpp"

namespace callscribe::ui {

// Builds and runs the full-screen FTXUI application. Blocks until the user
// quits. Returns a process exit code.
int run(AppContext& ctx);

}  // namespace callscribe::ui
