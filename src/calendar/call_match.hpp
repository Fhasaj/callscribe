#pragma once
#include <vector>

#include "calendar/ics_parser.hpp"
#include "db/repository.hpp"

namespace callscribe::calendar {

// Finds the recorded call that belongs to a calendar event, so the agenda can
// mark a meeting as done. Two ways a call is tied to an event:
//
//   1. Its recording overlaps the event's slot (allowing for joining early or
//      hitting record a few minutes late) -- the reliable signal, since it
//      doesn't depend on what the call was named.
//   2. Failing that, it was recorded on the same day and named after the event.
//
// Only calls that finished processing ("done") count: a call that errored out
// is precisely the one you'd want to see still outstanding. Returns nullptr
// when the meeting hasn't been recorded. The pointer is into `calls`.
const db::Call* recorded_call_for_event(const std::vector<db::Call>& calls, const Event& event);

}  // namespace callscribe::calendar
