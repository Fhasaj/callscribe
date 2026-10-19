#pragma once
#include <string>

#include "db/repository.hpp"

namespace callscribe::tasks {

// A plain-text account of one day, built from what was recorded: calls,
// tasks finished, focus time per project, wins, and what's still parked. It's
// what the Today tab shows and what a saved daily log keeps -- facts only, so
// the user's own note is the only part anyone had to write.
std::string build_day_digest(db::Repository& repo, const std::string& day);

}  // namespace callscribe::tasks
