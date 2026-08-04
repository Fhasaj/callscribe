#include "llm/summarizer.hpp"

#include <algorithm>

#include "llm/ollama_client.hpp"
#include "llm/prompts.hpp"

namespace callscribe::llm {

namespace {
std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// Splits on line boundaries so a slice never cuts mid-utterance.
std::vector<std::string> split_into_chunks(const std::string& text, size_t max_chars) {
    std::vector<std::string> chunks;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t remaining = text.size() - pos;
        if (remaining <= max_chars) {
            chunks.push_back(text.substr(pos));
            break;
        }
        size_t cut = text.rfind('\n', pos + max_chars);
        if (cut == std::string::npos || cut <= pos) cut = pos + max_chars;  // one enormous line
        chunks.push_back(text.substr(pos, cut - pos));
        pos = (cut < text.size() && text[cut] == '\n') ? cut + 1 : cut;
    }
    return chunks;
}

// Map step: condense each slice to bullets, then repeat if the combined bullets
// are still too long for one prompt.
std::string condense_transcript(const std::string& host, const std::string& model, const std::string& transcript,
                                 size_t budget, const std::function<void(const std::string&)>& on_progress) {
    // Slice well below the budget rather than right at it: each map call then
    // runs with a small context window (fast, modest VRAM) and the model has
    // less material to lose track of per slice.
    constexpr size_t kChunkChars = 40000;
    std::string current = transcript;
    int pass = 1;
    while (current.size() > budget) {
        std::vector<std::string> chunks = split_into_chunks(current, std::min(kChunkChars, budget));
        std::string condensed;
        for (size_t i = 0; i < chunks.size(); ++i) {
            if (on_progress) {
                on_progress("Condensing long transcript -- part " + std::to_string(i + 1) + " of " +
                            std::to_string(chunks.size()) + (pass > 1 ? " (pass " + std::to_string(pass) + ")" : ""));
            }
            std::string bullets = generate_text(
                host, model,
                build_transcript_chunk_prompt(chunks[i], static_cast<int>(i + 1), static_cast<int>(chunks.size())));
            if (!condensed.empty()) condensed += "\n";
            condensed += trim(bullets);
        }
        // Guard against a pass that fails to shrink anything, which would spin
        // forever; take what we have and let the caller work with it.
        if (condensed.size() >= current.size()) return condensed.substr(0, budget);
        current = condensed;
        ++pass;
    }
    return current;
}
}  // namespace

CallSummaryResult summarize_call(const std::string& host, const std::string& model, const std::string& transcript,
                                  const std::string& customer_name, const std::string& meeting_name,
                                  const std::string& participants, const std::string& call_date,
                                  const std::function<void(const std::string&)>& on_progress,
                                  const PromptPresets& prompts) {
    // Leave room for the instructions and metadata that wrap the transcript.
    const size_t budget = static_cast<size_t>(max_prompt_chars()) - 4096;
    std::string body = transcript;
    if (body.size() > budget) body = condense_transcript(host, model, body, budget, on_progress);

    if (on_progress) on_progress("Summarizing...");
    std::string prompt = build_summary_prompt(body, customer_name, meeting_name, participants, call_date,
                                               prompts.summary, prompts.email, prompts.tasks);
    nlohmann::json data = generate_json(host, model, prompt);

    std::vector<std::string> bullets;
    if (data.contains("summary")) {
        const auto& s = data.at("summary");
        if (s.is_array()) {
            for (const auto& b : s) bullets.push_back(b.is_string() ? b.get<std::string>() : b.dump());
        } else if (s.is_string()) {
            bullets.push_back(s.get<std::string>());
        }
    }

    CallSummaryResult result;
    for (const auto& b : bullets) {
        if (!result.summary_text.empty()) result.summary_text += "\n";
        result.summary_text += "- " + b;
    }

    if (data.contains("customer_email") && data.at("customer_email").is_string()) {
        result.email_draft = trim(data.at("customer_email").get<std::string>());
    }

    if (data.contains("tasks") && data.at("tasks").is_array()) {
        for (const auto& t : data.at("tasks")) {
            if (!t.is_string()) continue;
            std::string ts = trim(t.get<std::string>());
            if (!ts.empty()) result.tasks.push_back(ts);
        }
    }

    // Valid JSON with nothing in it used to surface as a successful run with a
    // blank summary pane and no explanation. Treat it as the failure it is.
    if (result.summary_text.empty() && result.email_draft.empty() && result.tasks.empty()) {
        throw OllamaError(
            "The model returned an empty summary. The transcript may be too short to summarize, or "
            "the model may not be following the JSON format -- try a different model in Settings.");
    }
    return result;
}

std::string generate_yearly_narrative(const std::string& host, const std::string& model, int year,
                                       const db::YearlyStats& stats) {
    std::string prompt = build_yearly_narrative_prompt(year, stats.summaries, stats.call_count, stats.total_seconds,
                                                         stats.tasks_completed, stats.top_customers);
    return generate_text(host, model, prompt);
}

std::string generate_review_report(const std::string& host, const std::string& model, int year,
                                    const std::string& cycle_label, const std::vector<std::string>& questions,
                                    const db::YearlyStats& stats) {
    std::string prompt = build_review_report_prompt(year, cycle_label, questions, stats.summaries, stats.call_count,
                                                     stats.total_seconds, stats.tasks_completed, stats.top_customers);
    return generate_text(host, model, prompt);
}

ChatReply proofread_email(const std::string& host, const std::string& model, const std::string& email_text,
                           const StreamCallback& on_progress) {
    return generate_reply(host, model, build_proofread_prompt(email_text), on_progress);
}

ChatReply chat_reply(const std::string& host, const std::string& model, const std::vector<ChatTurn>& history,
                      const std::string& message, const std::string& user_name,
                      const StreamCallback& on_progress) {
    return generate_reply(host, model, build_chat_prompt(history, message, user_name), on_progress);
}

}  // namespace callscribe::llm
