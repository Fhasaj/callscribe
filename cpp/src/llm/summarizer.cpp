#include "llm/summarizer.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

#include "llm/ollama_client.hpp"
#include "llm/prompts.hpp"
#include "util/date_format.hpp"
#include "util/time.hpp"

namespace callscribe::llm {

namespace {
std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Whatever the model wrote for a field: a string, or a number/bool/array it
// decided to use instead. Never the raw JSON dump, which would read as noise
// in the task list.
std::string as_text(const nlohmann::json& node) {
    if (node.is_null()) return "";
    if (node.is_string()) return trim(node.get<std::string>());
    if (node.is_number_integer()) return std::to_string(node.get<long long>());
    if (node.is_array() && !node.empty()) return as_text(node.front());
    if (node.is_boolean() || node.is_number()) return trim(node.dump());
    return "";
}

// Looks up a key case-insensitively and accepting the aliases models reach for
// ("date"/"deadline" for a due date, "client"/"account" for a customer).
std::string field(const nlohmann::json& obj, const std::vector<std::string>& names) {
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        std::string key = to_lower(it.key());
        for (const auto& name : names) {
            if (key == name || key.find(name) != std::string::npos) return as_text(it.value());
        }
    }
    return "";
}

// Strips "- ", "* ", "• ", "1. ", "1) " and surrounding bold markers off a
// line the model formatted as a bullet.
std::string strip_bullet(const std::string& line) {
    std::string s = trim(line);
    if (s.size() >= 2 && (s[0] == '-' || s[0] == '*' || s[0] == '+') && s[1] == ' ') s = trim(s.substr(2));
    if (s.rfind("\xE2\x80\xA2", 0) == 0) s = trim(s.substr(3));  // U+2022 bullet
    size_t i = 0;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
    if (i > 0 && i + 1 < s.size() && (s[i] == '.' || s[i] == ')') && s[i + 1] == ' ') s = trim(s.substr(i + 2));
    while (s.rfind("**", 0) == 0) s = trim(s.substr(2));
    while (s.size() >= 2 && s.compare(s.size() - 2, 2, "**") == 0) s = trim(s.substr(0, s.size() - 2));
    return s;
}

// Splits "| a | b | c |" into {"a", "b", "c"}.
std::vector<std::string> split_pipe_row(const std::string& line) {
    std::vector<std::string> cells;
    std::string cell;
    for (char c : line) {
        if (c == '|') {
            cells.push_back(trim(cell));
            cell.clear();
        } else {
            cell += c;
        }
    }
    cells.push_back(trim(cell));
    if (!cells.empty() && cells.front().empty()) cells.erase(cells.begin());
    if (!cells.empty() && cells.back().empty()) cells.pop_back();
    return cells;
}

bool is_table_separator(const std::string& line) {
    bool saw_dash = false;
    for (char c : line) {
        if (c == '-') saw_dash = true;
        else if (c != '|' && c != ':' && c != ' ' && c != '\t' && c != '+' && c != '=')
            return false;
    }
    return saw_dash;
}

// The model ignored the object schema and answered with a markdown table (the
// common failure: every task ends up in one string). Recovers one task per row
// by reading the header for which column is which.
std::vector<ExtractedTask> parse_markdown_table(const std::vector<std::string>& rows) {
    int desc_col = 0, due_col = -1, prio_col = -1, cust_col = -1;
    size_t first_data_row = 0;
    bool header_seen = false;

    std::vector<std::string> header = split_pipe_row(rows.front());
    for (size_t i = 0; i < header.size(); ++i) {
        std::string h = to_lower(header[i]);
        if (h.find("task") != std::string::npos || h.find("description") != std::string::npos ||
            h.find("action") != std::string::npos) {
            desc_col = static_cast<int>(i);
            header_seen = true;
        } else if (h.find("due") != std::string::npos || h.find("date") != std::string::npos ||
                   h.find("deadline") != std::string::npos) {
            due_col = static_cast<int>(i);
            header_seen = true;
        } else if (h.find("prior") != std::string::npos) {
            prio_col = static_cast<int>(i);
            header_seen = true;
        } else if (h.find("customer") != std::string::npos || h.find("client") != std::string::npos ||
                   h.find("account") != std::string::npos || h.find("company") != std::string::npos) {
            cust_col = static_cast<int>(i);
            header_seen = true;
        }
    }
    if (header_seen) first_data_row = 1;

    std::vector<ExtractedTask> out;
    for (size_t r = first_data_row; r < rows.size(); ++r) {
        if (is_table_separator(rows[r])) continue;
        std::vector<std::string> cells = split_pipe_row(rows[r]);
        if (cells.empty()) continue;
        ExtractedTask task;
        if (desc_col < static_cast<int>(cells.size())) task.description = strip_bullet(cells[desc_col]);
        if (due_col >= 0 && due_col < static_cast<int>(cells.size())) task.due_date = cells[due_col];
        if (prio_col >= 0 && prio_col < static_cast<int>(cells.size())) task.priority = cells[prio_col];
        if (cust_col >= 0 && cust_col < static_cast<int>(cells.size())) task.customer = cells[cust_col];
        if (!task.description.empty()) out.push_back(std::move(task));
    }
    return out;
}

// A task value that came back as free text rather than an object -- either one
// task per line, or a whole markdown table stuffed into a single string.
std::vector<ExtractedTask> parse_task_text(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!trim(line).empty()) lines.push_back(trim(line));
    }
    if (lines.empty()) return {};

    size_t pipe_rows = 0;
    for (const auto& l : lines) {
        if (std::count(l.begin(), l.end(), '|') >= 2) ++pipe_rows;
    }
    if (pipe_rows >= lines.size() / 2 && pipe_rows > 0) return parse_markdown_table(lines);

    std::vector<ExtractedTask> out;
    for (const auto& l : lines) {
        std::string desc = strip_bullet(l);
        if (!desc.empty()) out.push_back(ExtractedTask{desc, "", db::kPriorityMedium, ""});
    }
    return out;
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

std::vector<ExtractedTask> parse_task_items(const nlohmann::json& node, const std::string& default_customer,
                                             const std::string& reference_date) {
    std::vector<ExtractedTask> raw;

    auto absorb_object = [&](const nlohmann::json& obj) {
        ExtractedTask task;
        task.description = field(obj, {"description", "task", "action", "item", "title", "what"});
        task.due_date = field(obj, {"due_date", "due", "date", "deadline"});
        task.priority = field(obj, {"priority", "urgency", "importance"});
        task.customer = field(obj, {"customer", "client", "account", "company"});
        if (task.description.empty()) return;
        // A "description" that is itself a table or a bullet list is still
        // several tasks wearing one coat.
        std::vector<ExtractedTask> nested = parse_task_text(task.description);
        if (nested.size() > 1) {
            for (auto& n : nested) {
                if (n.due_date.empty()) n.due_date = task.due_date;
                if (n.priority.empty() || n.priority == db::kPriorityMedium) n.priority = task.priority;
                if (n.customer.empty()) n.customer = task.customer;
                raw.push_back(std::move(n));
            }
            return;
        }
        if (!nested.empty()) task.description = nested.front().description;
        raw.push_back(std::move(task));
    };

    if (node.is_array()) {
        for (const auto& item : node) {
            if (item.is_object()) {
                absorb_object(item);
            } else if (item.is_string()) {
                for (auto& t : parse_task_text(item.get<std::string>())) raw.push_back(std::move(t));
            }
        }
    } else if (node.is_string()) {
        raw = parse_task_text(node.get<std::string>());
    } else if (node.is_object()) {
        // Either a single task, or a wrapper like {"tasks": [...]}.
        if (!field(node, {"description", "task", "action", "item", "title"}).empty()) {
            absorb_object(node);
        } else {
            for (auto it = node.begin(); it != node.end(); ++it) {
                for (auto& t : parse_task_items(it.value(), default_customer, reference_date)) raw.push_back(std::move(t));
            }
        }
    }

    const std::string base_date = reference_date.empty() ? util::today_iso() : reference_date;
    std::vector<ExtractedTask> out;
    for (auto& task : raw) {
        task.description = strip_bullet(task.description);
        if (task.description.empty()) continue;
        task.due_date = util::normalize_due_date(task.due_date, base_date);
        task.priority = db::normalize_priority(task.priority);
        if (to_lower(task.customer).find("not specified") != std::string::npos || task.customer == "-" ||
            to_lower(task.customer) == "n/a" || to_lower(task.customer) == "unknown")
            task.customer.clear();
        if (task.customer.empty()) task.customer = default_customer;
        out.push_back(std::move(task));
    }
    return out;
}

CallSummaryResult summarize_call(const std::string& host, const std::string& model, const std::string& transcript,
                                  const std::string& customer_name, const std::string& meeting_name,
                                  const std::string& participants, const std::string& call_date,
                                  const std::function<void(const std::string&)>& on_progress,
                                  const PromptPresets& prompts) {
    // One session for the whole map/reduce chain below: the model stays loaded
    // between the chunk calls and the final summary, then goes away.
    ModelSession session(host, model);
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

    if (data.contains("tasks")) {
        std::string reference = util::display_date_to_iso(call_date);
        if (reference.empty()) reference = util::normalize_due_date(call_date, util::today_iso());
        result.tasks = parse_task_items(data.at("tasks"), customer_name, reference);
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
    ModelSession session(host, model);
    return generate_text(host, model, prompt);
}

std::string generate_review_report(const std::string& host, const std::string& model, int year,
                                    const std::string& cycle_label, const std::vector<std::string>& questions,
                                    const db::YearlyStats& stats) {
    std::string prompt = build_review_report_prompt(year, cycle_label, questions, stats.summaries, stats.call_count,
                                                     stats.total_seconds, stats.tasks_completed, stats.top_customers);
    ModelSession session(host, model);
    return generate_text(host, model, prompt);
}

ChatReply proofread_email(const std::string& host, const std::string& model, const std::string& email_text,
                           const StreamCallback& on_progress) {
    ModelSession session(host, model);
    return generate_reply(host, model, build_proofread_prompt(email_text), on_progress);
}

ChatReply chat_reply(const std::string& host, const std::string& model, const std::vector<ChatTurn>& history,
                      const std::string& message, const std::string& user_name,
                      const StreamCallback& on_progress) {
    // A chat turn is a single reply, so the model is loaded per message rather
    // than parked between them. Reloading costs seconds; sitting on gigabytes
    // while the user types their next message costs more.
    ModelSession session(host, model);
    return generate_reply(host, model, build_chat_prompt(history, message, user_name), on_progress);
}

}  // namespace callscribe::llm
