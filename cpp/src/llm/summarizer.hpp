#pragma once
#include <string>
#include <vector>

#include "db/repository.hpp"

namespace callscribe::llm {

struct CallSummaryResult {
    std::string summary_text;
    std::string email_draft;
    std::vector<std::string> tasks;
};

CallSummaryResult summarize_call(const std::string& host, const std::string& model, const std::string& transcript,
                                  const std::string& customer_name, const std::string& meeting_name,
                                  const std::string& participants, const std::string& call_date);

std::string generate_yearly_narrative(const std::string& host, const std::string& model, int year,
                                       const db::YearlyStats& stats);

std::string proofread_email(const std::string& host, const std::string& model, const std::string& email_text);

}  // namespace callscribe::llm
