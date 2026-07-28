#pragma once
#include <functional>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>

namespace callscribe::llm {

class OllamaError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

bool is_running(const std::string& host);
std::vector<std::string> list_models(const std::string& host);
void pull_model(const std::string& host, const std::string& model,
                 const std::function<void(const std::string&)>& on_progress = nullptr);

// Runs a JSON-mode generate call and returns the parsed inner object
// (falls back to extracting the outermost {...} if the model wraps its
// JSON in stray text).
nlohmann::json generate_json(const std::string& host, const std::string& model, const std::string& prompt,
                              long timeout_seconds = 300);
std::string generate_text(const std::string& host, const std::string& model, const std::string& prompt,
                           long timeout_seconds = 300);

}  // namespace callscribe::llm
