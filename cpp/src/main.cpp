#include <cstdio>

#include "context.hpp"
#include "ui/app.hpp"

int main() {
    try {
        callscribe::AppContext ctx;
        return callscribe::ui::run(ctx);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "CallScribe failed to start: %s\n", e.what());
        return 1;
    }
}
