#include "llm/summarizer.hpp"

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
}  // namespace

CallSummaryResult summarize_call(const std::string& host, const std::string& model, const std::string& transcript,
                                  const std::string& customer_name, const std::string& meeting_name,
                                  const std::string& participants, const std::string& call_date) {
    std::string prompt = build_summary_prompt(transcript, customer_name, meeting_name, participants, call_date);
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
    return result;
}

std::string generate_yearly_narrative(const std::string& host, const std::string& model, int year,
                                       const db::YearlyStats& stats) {
    std::string prompt = build_yearly_narrative_prompt(year, stats.summaries, stats.call_count, stats.total_seconds,
                                                         stats.tasks_completed, stats.top_customers);
    return generate_text(host, model, prompt);
}

std::string proofread_email(const std::string& host, const std::string& model, const std::string& email_text) {
    return generate_text(host, model, build_proofread_prompt(email_text));
}

}  // namespace callscribe::llm
