#include <doctest/doctest.h>

#include "transcription/diarizer.hpp"

using namespace callscribe::transcription;

namespace {
DiarizedSegment seg(int64_t start_ms, int64_t end_ms, int speaker) {
    return DiarizedSegment{start_ms, end_ms, speaker};
}
}  // namespace

TEST_CASE("label_lines: names follow the order voices are first heard") {
    // Cluster ids come back in whatever order the clusterer produced them --
    // here the first voice heard is cluster 7, not cluster 0.
    std::vector<DiarizedSegment> speech = {seg(0, 1000, 7), seg(1000, 2000, 3), seg(2000, 3000, 7)};
    std::vector<std::pair<int64_t, int64_t>> lines = {{0, 900}, {1100, 1900}, {2100, 2900}};

    auto names = label_lines(speech, lines, {"Alice", "Bob"});
    CHECK(names[0] == "Alice");
    CHECK(names[1] == "Bob");
    CHECK(names[2] == "Alice");
}

TEST_CASE("label_lines: voices beyond the participant list are numbered, not guessed") {
    std::vector<DiarizedSegment> speech = {seg(0, 1000, 0), seg(1000, 2000, 1), seg(2000, 3000, 2)};
    std::vector<std::pair<int64_t, int64_t>> lines = {{0, 900}, {1100, 1900}, {2100, 2900}};

    auto names = label_lines(speech, lines, {"Alice"});
    CHECK(names[0] == "Alice");
    CHECK(names[1] == "Speaker 2");
    CHECK(names[2] == "Speaker 3");

    // With no names at all, everything is numbered.
    auto anonymous = label_lines(speech, lines, {});
    CHECK(anonymous[0] == "Speaker 1");
    CHECK(anonymous[2] == "Speaker 3");
}

TEST_CASE("label_lines: a line straddling a handover goes to whoever said more of it") {
    std::vector<DiarizedSegment> speech = {seg(0, 1000, 0), seg(1000, 5000, 1)};
    // 200ms of the first speaker, 3000ms of the second.
    std::vector<std::pair<int64_t, int64_t>> lines = {{800, 4000}};

    auto names = label_lines(speech, lines, {"Alice", "Bob"});
    CHECK(names[0] == "Bob");
}

TEST_CASE("label_lines: a line with no overlapping speech is left alone") {
    std::vector<DiarizedSegment> speech = {seg(0, 1000, 0)};
    std::vector<std::pair<int64_t, int64_t>> lines = {{5000, 6000}};

    auto names = label_lines(speech, lines, {"Alice"});
    // Empty, so the caller keeps the label the line already had rather than
    // attributing it to the only speaker it happens to know about.
    CHECK(names[0].empty());
}

TEST_CASE("label_lines: no diarized speech at all leaves every line alone") {
    std::vector<std::pair<int64_t, int64_t>> lines = {{0, 1000}, {1000, 2000}};
    auto names = label_lines({}, lines, {"Alice", "Bob"});
    REQUIRE(names.size() == 2);
    CHECK(names[0].empty());
    CHECK(names[1].empty());
}

TEST_CASE("label_lines: touching but non-overlapping ranges do not count as a match") {
    // Ranges are half-open; a line starting exactly where a segment ends
    // shares no audio with it.
    std::vector<DiarizedSegment> speech = {seg(0, 1000, 0)};
    std::vector<std::pair<int64_t, int64_t>> lines = {{1000, 2000}};
    auto names = label_lines(speech, lines, {"Alice"});
    CHECK(names[0].empty());
}
