#pragma once
#include <string>

namespace callscribe::util {

// "2026-07-28" -> "28 07 2026". Dates stay stored/compared as ISO
// internally (needed for correct lexicographic sorting/filtering in
// SQL) -- this is purely for display. Returns the input unchanged if it
// doesn't parse as an ISO date.
std::string iso_date_to_display(const std::string& iso_date);

// "28 07 2026" / "28-07-2026" / "28/07/2026" -> "2026-07-28". Returns ""
// if the input doesn't parse as a day/month/year triple.
std::string display_date_to_iso(const std::string& display_date);

// Best-effort conversion of a due date written by a person or a model into
// ISO. Accepts ISO as-is, DD MM YYYY, and relative wording ("today",
// "tomorrow", "next week", "friday", "next friday") resolved against
// `reference_iso` -- so an LLM that answers "Tomorrow" still produces a real
// date to sort and remind on. Returns "" for no/unparseable date, including
// the "not specified" style placeholders models like to emit.
std::string normalize_due_date(const std::string& text, const std::string& reference_iso);

}  // namespace callscribe::util
