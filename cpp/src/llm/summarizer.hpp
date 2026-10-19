#pragma once
#include <functional>
#include <string>
#include <vector>

#include "db/repository.hpp"
#include "llm/ollama_client.hpp"
#include "llm/prompts.hpp"

namespace callscribe::llm {

// One action item pulled out of a call. `due_date` is ISO (or empty for "no
// date"), `priority` is one of db::kPriority*, and `customer` is the company
// the task belongs to (usually, but not always, the call's own customer).
struct ExtractedTask {
    std::string description;
    std::string due_date;
    std::string priority = db::kPriorityMedium;
    std::string customer;
    // The deadline in the words used on the call ("by Friday"), when the model
    // reports it -- resolved here rather than trusting the model's own date.
    std::string due_said;
};

struct CallSummaryResult {
    std::string summary_text;
    std::string email_draft;
    std::vector<ExtractedTask> tasks;
};

// Turns whatever the model put in the "tasks" key into one ExtractedTask per
// action. Models routinely ignore the object schema and answer with plain
// strings, a numbered list crammed into one string, or a markdown table -- all
// of which used to land in the DB as a single unusable "task" -- so each of
// those shapes is unpicked here instead. `default_customer` fills in tasks
// with no customer of their own, and `reference_date` (ISO) is what relative
// due dates like "tomorrow" are resolved against.
std::vector<ExtractedTask> parse_task_items(const nlohmann::json& node, const std::string& default_customer,
                                             const std::string& reference_date);

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
                                  const PromptPresets& prompts = {}, const std::string& user_name = "");

std::string generate_yearly_narrative(const std::string& host, const std::string& model, int year,
                                       const db::YearlyStats& stats);

// Answers performance-review questions from everything recorded in the
// period: picks as much of `evidence` as the model's context holds (see
// select_evidence) and writes paste-ready answers. `cycle_label` is
// "semi-annual", "annual", or "" for a question of the user's own.
// Everything recorded for one calendar year, as review material: the facts
// and one evidence item per call note, completed task, win, noted focus round
// and daily note. Wins and daily notes are pinned (see EvidenceItem).
struct ReviewMaterial {
    ReviewFacts facts;
    std::vector<EvidenceItem> evidence;
};
ReviewMaterial collect_review_material(db::Repository& repo, int year);

// Splits pasted review questions apart: on blank lines, or -- for a block of
// one-line questions with no blank lines between them -- one per line.
std::vector<std::string> split_questions(const std::string& text);

struct ReviewReport {
    std::string text;
    size_t evidence_used = 0;
    size_t evidence_total = 0;
};
ReviewReport generate_review_report(const std::string& host, const std::string& model, const std::string& user_name,
                                    const std::string& cycle_label, const std::vector<std::string>& questions,
                                    const ReviewFacts& facts, std::vector<EvidenceItem> evidence);

// Pulls the step list out of a breakdown reply: {"steps": [...]} of strings
// or of objects with a "step"/"description"/"title", tolerating the same
// list-in-one-string shapes parse_task_items() does. Empty entries dropped.
std::vector<std::string> parse_steps(const nlohmann::json& reply);

// Asks the model for the first few concrete steps of `task`.
std::vector<std::string> break_down_task(const std::string& host, const std::string& model, const std::string& task,
                                         const std::string& customer, const std::string& due_date);

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
