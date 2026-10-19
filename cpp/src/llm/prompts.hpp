#pragma once
#include <string>
#include <utility>
#include <vector>

#include "llm/evidence.hpp"

namespace callscribe::llm {

// `summary_instruction` / `email_instruction` / `tasks_instruction` are the
// user's editable presets from Settings; the surrounding JSON contract is fixed
// because the reply has to be parseable. Empty strings fall back to defaults.
std::string build_summary_prompt(const std::string& transcript, const std::string& customer_name,
                                  const std::string& meeting_name, const std::string& participants,
                                  const std::string& call_date, const std::string& summary_instruction = "",
                                  const std::string& email_instruction = "",
                                  const std::string& tasks_instruction = "", const std::string& user_name = "");

// Condenses one slice of an over-long transcript into bullets, so the slices
// can be summarized together in a second pass.
std::string build_transcript_chunk_prompt(const std::string& chunk, int index, int total);

std::string build_yearly_narrative_prompt(int year, const std::vector<std::string>& summaries, int call_count,
                                           long long total_seconds, int tasks_completed,
                                           const std::vector<std::pair<std::string, int>>& top_customers);

// The headline numbers for a review period, stated as facts in the prompt so
// the model quotes them rather than estimating.
struct ReviewFacts {
    std::string period;  // e.g. "2026"
    int call_count = 0;
    long long call_seconds = 0;
    int tasks_completed = 0;
    int wins = 0;
    std::vector<std::pair<std::string, int>> top_customers;          // name, calls
    std::vector<std::pair<std::string, int>> focus_seconds_by_tag;  // tag, seconds
};

// Performance-review answers. `cycle_label` is e.g. "semi-annual", "annual" or
// "" for a one-off question; `evidence` is what select_evidence() picked, and
// `evidence_total` how many items there were before it had to choose.
std::string build_review_report_prompt(const std::string& user_name, const std::string& cycle_label,
                                       const std::vector<std::string>& questions, const ReviewFacts& facts,
                                       const std::vector<EvidenceItem>& evidence, size_t evidence_total);

std::string build_proofread_prompt(const std::string& email_text);

// Turns one vague task into a few small, concrete first steps -- the kind you
// can start without having to think about how. Asks for {"steps": [...]}.
std::string build_task_breakdown_prompt(const std::string& task, const std::string& customer,
                                        const std::string& due_date);

// One turn of the Chat screen's transcript.
struct ChatTurn {
    bool is_user;
    std::string text;
};

// Free-form assistant chat. `history` is the conversation so far (oldest
// first, excluding `message`); the oldest turns are dropped if the whole thing
// would not fit the model's context window.
std::string build_chat_prompt(const std::vector<ChatTurn>& history, const std::string& message,
                               const std::string& user_name = "");

}  // namespace callscribe::llm
