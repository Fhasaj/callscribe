#pragma once
#include <string>
#include <utility>
#include <vector>

namespace callscribe::llm {

// `summary_instruction` / `email_instruction` / `tasks_instruction` are the
// user's editable presets from Settings; the surrounding JSON contract is fixed
// because the reply has to be parseable. Empty strings fall back to defaults.
std::string build_summary_prompt(const std::string& transcript, const std::string& customer_name,
                                  const std::string& meeting_name, const std::string& participants,
                                  const std::string& call_date, const std::string& summary_instruction = "",
                                  const std::string& email_instruction = "",
                                  const std::string& tasks_instruction = "");

// Condenses one slice of an over-long transcript into bullets, so the slices
// can be summarized together in a second pass.
std::string build_transcript_chunk_prompt(const std::string& chunk, int index, int total);

std::string build_yearly_narrative_prompt(int year, const std::vector<std::string>& summaries, int call_count,
                                           long long total_seconds, int tasks_completed,
                                           const std::vector<std::pair<std::string, int>>& top_customers);

// KPI report. `cycle_label` is e.g. "semi-annual" or "annual" and `kpis` are
// the KPIs the user asked to have tracked; the model reports on each one from
// the year's activity and the call history in `summaries`. Asks for JSON with
// "report", "email_subject" and "email_body" so the same run yields both the
// long-form report and a ready-to-send email.
std::string build_kpi_report_prompt(int year, const std::string& cycle_label,
                                     const std::vector<std::string>& kpis,
                                     const std::vector<std::string>& summaries, int call_count,
                                     long long total_seconds, int tasks_completed,
                                     const std::vector<std::pair<std::string, int>>& top_customers);

std::string build_proofread_prompt(const std::string& email_text);

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
