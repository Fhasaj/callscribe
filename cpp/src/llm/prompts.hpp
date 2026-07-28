#pragma once
#include <string>
#include <utility>
#include <vector>

namespace callscribe::llm {

std::string build_summary_prompt(const std::string& transcript, const std::string& customer_name,
                                  const std::string& meeting_name, const std::string& participants,
                                  const std::string& call_date);

std::string build_yearly_narrative_prompt(int year, const std::vector<std::string>& summaries, int call_count,
                                           long long total_seconds, int tasks_completed,
                                           const std::vector<std::pair<std::string, int>>& top_customers);

std::string build_proofread_prompt(const std::string& email_text);

}  // namespace callscribe::llm
