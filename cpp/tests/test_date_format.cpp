#include <doctest/doctest.h>

#include "util/date_format.hpp"

using namespace callscribe::util;

TEST_CASE("iso_date_to_display converts YYYY-MM-DD to DD MM YYYY") {
    CHECK(iso_date_to_display("2026-07-28") == "28 07 2026");
    CHECK(iso_date_to_display("2026-01-05") == "05 01 2026");
}

TEST_CASE("iso_date_to_display passes through unparsable input") {
    CHECK(iso_date_to_display("") == "");
    CHECK(iso_date_to_display("not-a-date") == "not-a-date");
}

TEST_CASE("display_date_to_iso accepts space, dash, and slash separators") {
    CHECK(display_date_to_iso("28 07 2026") == "2026-07-28");
    CHECK(display_date_to_iso("28-07-2026") == "2026-07-28");
    CHECK(display_date_to_iso("28/07/2026") == "2026-07-28");
}

TEST_CASE("display_date_to_iso rejects invalid dates") {
    CHECK(display_date_to_iso("") == "");
    CHECK(display_date_to_iso("not a date") == "");
    CHECK(display_date_to_iso("32 13 2026") == "");
}

TEST_CASE("date round-trips through both conversions") {
    CHECK(display_date_to_iso(iso_date_to_display("2026-07-28")) == "2026-07-28");
}

TEST_CASE("spoken deadlines resolve to the right day, vague ones don't") {
    const std::string monday = "2026-10-19";
    CHECK(resolve_spoken_deadline("by Friday", monday) == "2026-10-23");
    CHECK(resolve_spoken_deadline("Friday.", monday) == "2026-10-23");
    CHECK(resolve_spoken_deadline("tomorrow", monday) == "2026-10-20");
    CHECK(resolve_spoken_deadline("on Thursday", monday) == "2026-10-22");
    CHECK(resolve_spoken_deadline("by the end of the week", monday) == "2026-10-23");
    CHECK(resolve_spoken_deadline("end of day", monday) == "2026-10-19");
    CHECK(resolve_spoken_deadline("next week", monday).empty());
    CHECK(resolve_spoken_deadline("soon", monday).empty());
    CHECK(resolve_spoken_deadline("", monday).empty());
}
