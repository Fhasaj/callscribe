#include "llm/evidence.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace callscribe::llm {

namespace {

const std::set<std::string>& stopwords() {
    static const std::set<std::string> words = {
        "the",  "and",   "for",   "are",   "but",   "not",  "you",   "your",  "with", "what", "when", "where",
        "which", "who",  "why",   "how",   "this",  "that", "these", "those", "have", "has",  "had",  "was",
        "were", "been",  "being", "from",  "into",  "about", "some", "any",   "all",  "can",  "could", "would",
        "should", "will", "did",  "does",  "doing", "done", "their", "they",  "them", "there", "here", "our",
        "out",  "over",  "during", "since", "than", "then", "also",  "more",  "most", "very", "just", "much",
        "many", "such",  "each",  "other", "time",  "times", "way",  "give",  "describe", "tell", "share",
        "example", "examples", "please", "year", "review", "cycle", "period", "past", "last", "things",
        "thing", "well", "like", "make", "made", "take", "took", "get", "got",
    };
    return words;
}

std::string lower(const std::string& s) {
    std::string out = s;
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// Crude stemming, enough that "improve" finds "improved" and "improvements".
std::string stem(const std::string& w) {
    for (const char* suffix : {"ments", "ment", "ings", "ing", "ies", "ed", "es", "s"}) {
        std::string suf = suffix;
        if (w.size() > suf.size() + 3 && w.compare(w.size() - suf.size(), suf.size(), suf) == 0)
            return w.substr(0, w.size() - suf.size());
    }
    return w;
}

}  // namespace

std::vector<std::string> keywords(const std::string& text) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    std::string word;
    auto flush = [&] {
        if (word.size() >= 3 && !stopwords().count(word)) {
            std::string s = stem(word);
            if (seen.insert(s).second) out.push_back(s);
        }
        word.clear();
    };
    for (char c : lower(text)) {
        if (std::isalnum(static_cast<unsigned char>(c)))
            word += c;
        else
            flush();
    }
    flush();
    return out;
}

std::string render_evidence_line(const EvidenceItem& item) {
    std::string text = item.text;
    // One line per item: newlines inside a summary become separators.
    for (auto& c : text) {
        if (c == '\n' || c == '\r') c = ' ';
    }
    return item.date.substr(0, 10) + " · " + item.kind + " · " + text;
}

std::vector<EvidenceItem> select_evidence(std::vector<EvidenceItem> items, const std::string& query,
                                          size_t budget_chars) {
    const auto terms = keywords(query);

    struct Ranked {
        size_t index;
        int matches;
        int month_rank;  // 0 = newest in its month, 1 = next...
    };
    std::vector<Ranked> ranked;
    ranked.reserve(items.size());
    std::map<std::string, int> per_month;

    // Newest first, so month_rank 0 is the latest item of each month.
    std::vector<size_t> order(items.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return items[a].date > items[b].date; });

    for (size_t i : order) {
        const std::string text = lower(items[i].text);
        int matches = 0;
        for (const auto& t : terms) {
            if (text.find(t) != std::string::npos) ++matches;
        }
        ranked.push_back({i, matches, per_month[items[i].date.substr(0, 7)]++});
    }

    // Pinned, then relevance, then breadth: the newest item of every month
    // before the second-newest of any, newest month first within a rank.
    std::stable_sort(ranked.begin(), ranked.end(), [&](const Ranked& a, const Ranked& b) {
        if (items[a.index].pinned != items[b.index].pinned) return items[a.index].pinned;
        if (a.matches != b.matches) return a.matches > b.matches;
        if (a.month_rank != b.month_rank) return a.month_rank < b.month_rank;
        return items[a.index].date > items[b.index].date;
    });

    std::vector<EvidenceItem> chosen;
    size_t used = 0;
    for (const auto& r : ranked) {
        const size_t cost = render_evidence_line(items[r.index]).size() + 1;
        if (used + cost > budget_chars) continue;  // a shorter one further down may still fit
        used += cost;
        chosen.push_back(std::move(items[r.index]));
    }
    std::sort(chosen.begin(), chosen.end(),
              [](const EvidenceItem& a, const EvidenceItem& b) { return a.date < b.date; });
    return chosen;
}

}  // namespace callscribe::llm
