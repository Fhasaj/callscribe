#pragma once
#include <functional>
#include <string>
#include <vector>

#include "db/repository.hpp"
#include "llm/ollama_client.hpp"
#include "llm/prompts.hpp"

namespace callscribe::llm {

struct CallSummaryResult {
    std::string summary_text;
    std::string email_draft;
    std::vector<std::string> tasks;
};

// Summarizes a call. Transcripts too long for the model's context window are
// condensed in slices first and the slice bullets summarized together, so an
// hours-long call still produces notes instead of silently truncated ones.
// `on_progress` (optional) reports the current phase for the UI to display;
// it is called from the calling thread.
// `prompts` carries the user's editable Settings presets straight through to
// build_summary_prompt; default-constructed means "use the built-in wording".
struct PromptPresets {
    std::string summary;
    std::string email;
    std::string tasks;
};

CallSummaryResult summarize_call(const std::string& host, const std::string& model, const std::string& transcript,
                                  const std::string& customer_name, const std::string& meeting_name,
                                  const std::string& participants, const std::string& call_date,
                                  const std::function<void(const std::string&)>& on_progress = nullptr,
                                  const PromptPresets& prompts = {});

std::string generate_yearly_narrative(const std::string& host, const std::string& model, int year,
                                       const db::YearlyStats& stats);

// Answers the review questions picked in the Yearly Summary wizard against the
// year's call history. `cycle_label` is "semi-annual" or "annual".
std::string generate_review_report(const std::string& host, const std::string& model, int year,
                                    const std::string& cycle_label, const std::vector<std::string>& questions,
                                    const db::YearlyStats& stats);

// The two Chat-screen entry points. Both return the model's reasoning
// alongside the answer (empty for models that don't reason) and report
// streaming progress, so the screen can show a live indicator and a
// foldaway "thinking" section instead of a blank wait.
llm::ChatReply proofread_email(const std::string& host, const std::string& model, const std::string& email_text,
                                const StreamCallback& on_progress = nullptr);

// Free-form chat reply, given the conversation so far. See build_chat_prompt.
llm::ChatReply chat_reply(const std::string& host, const std::string& model, const std::vector<ChatTurn>& history,
                           const std::string& message, const std::string& user_name = "",
                           const StreamCallback& on_progress = nullptr);

}  // namespace callscribe::llm
