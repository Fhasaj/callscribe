#include <doctest/doctest.h>

#include <string>

#include "ui/text_wrap.hpp"

using namespace callscribe::ui;

TEST_CASE("WrapText breaks on word boundaries and keeps blank lines") {
    const std::string source = "the quick brown fox\n\njumps over";
    CHECK(WrapText(source, 10) == "the quick\nbrown fox\n\njumps over");
    CHECK(WrappedLineCount(source, 10) == 4);
}

TEST_CASE("WrapText keeps the indent on continuation lines") {
    CHECK(WrapText("  - alpha beta gamma", 12) == "  - alpha\n  beta gamma");
}

TEST_CASE("WrapText hard-splits a word longer than the line") {
    CHECK(WrapText("aaaaaaaa", 3) == "aaa\naaa\naa");
}

TEST_CASE("WrapText leaves the content alone before the pane is measured") {
    CHECK(WrapText("a much longer line than one column", 1) == "a much longer line than one column");
}

TEST_CASE("UnwrapSelection rejoins the breaks wrapping inserted") {
    const std::string source =
        "I am just following up on this email to see when you would like to schedule training.";
    const std::string wrapped = WrapText(source, 30);
    REQUIRE(wrapped.find('\n') != std::string::npos);
    CHECK(UnwrapSelection(wrapped) == source);
}

TEST_CASE("UnwrapSelection keeps the author's own line breaks") {
    const std::string source = "Hi Tom,\n\n- first item\n- second item\n\nBest regards,\nFatlum";
    CHECK(UnwrapSelection(WrapText(source, 40)) == source);
}

TEST_CASE("UnwrapSelection tolerates the padding the screen adds to a selection") {
    const std::string source = "alpha beta gamma delta";
    std::string wrapped = WrapText(source, 12);  // "alpha beta\ngamma delta"
    REQUIRE(wrapped == "alpha beta\ngamma delta");
    CHECK(UnwrapSelection("alpha beta  \ngamma delta") == source);
}

TEST_CASE("UnwrapSelection leaves text it never wrapped untouched") {
    CHECK(UnwrapSelection("one\ntwo\nthree") == "one\ntwo\nthree");
}
