#pragma once
#include <string>
#include <vector>

// Choosing which of a year's notes go into a review prompt. A year of call
// summaries, wins, focus notes and daily logs is far more than a local model's
// context holds, so this picks what fits -- favouring what the question is
// about, and otherwise spreading the picks across the year so a generic
// question ("what do I do well?") isn't answered from December alone.
namespace callscribe::llm {

struct EvidenceItem {
    std::string date;  // ISO date or timestamp; sorts as a string
    std::string kind;  // "Call", "Win", "Task done", "Focus", "Day"
    std::string text;
    // Always worth including first: wins and the user's own daily notes are
    // short, deliberate, and exactly what a review answer is built from.
    bool pinned = false;
};

// Lower-cased words of 3+ letters, minus common filler -- what a question is
// actually about.
std::vector<std::string> keywords(const std::string& text);

// Picks items until `budget_chars` (counting each as it will be rendered) is
// used up: pinned ones first, then by how many of the query's keywords they
// mention, then evenly across months. Returned oldest first.
std::vector<EvidenceItem> select_evidence(std::vector<EvidenceItem> items, const std::string& query,
                                          size_t budget_chars);

// "2026-03-14 · Call · <text>" -- the line format the prompt uses.
std::string render_evidence_line(const EvidenceItem& item);

}  // namespace callscribe::llm
