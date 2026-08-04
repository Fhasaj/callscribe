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

}  // namespace callscribe::util
