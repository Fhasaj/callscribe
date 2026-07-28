// Manual integration test (not part of ctest): exercises the real
// non-GUI pipeline end-to-end -- captures audio via parec on the default
// monitor source while a synthesized test call plays through the
// speaker, transcribes it with the real downloaded Whisper model, runs
// it through a real local Ollama call, and writes to a temp SQLite DB.
// Run directly: ./build/integration_pipeline_test
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

#include "audio/capture.hpp"
#include "audio/devices.hpp"
#include "config.hpp"
#include "db/database.hpp"
#include "db/repository.hpp"
#include "llm/summarizer.hpp"
#include "transcription/whisper_engine.hpp"

using namespace callscribe;

namespace {
void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("CHECK FAILED: " + message);
}
}  // namespace

int main() {
    const std::string test_wav = "/tmp/callscribe_cpp_pipeline_test.wav";
    std::cout << "Generating test speech with espeak-ng...\n";
    int rc = std::system(
        ("espeak-ng -w " + test_wav +
         " \"Hi Sarah, this is a test call about the quarterly renewal. Please email the updated contract to "
         "the customer by Monday and schedule a follow up call next week.\"")
            .c_str());
    check(rc == 0, "espeak-ng failed to generate test audio");

    std::cout << "Loading Whisper model...\n";
    std::string size = transcription::suggest_model_size();
    transcription::ensure_model_downloaded(size);
    std::string model_path = (models_dir() / transcription::model_filename(size)).string();
    transcription::WhisperEngine engine(model_path);

    std::cout << "Starting audio capture on the default monitor source...\n";
    std::string monitor_source = audio::default_monitor_source();
    check(!monitor_source.empty(), "could not resolve a default monitor source -- is PipeWire/Pulse running?");
    audio::StreamCapture capture(monitor_source);
    capture.start();

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    rc = std::system(("paplay " + test_wav).c_str());
    check(rc == 0, "paplay failed to play test audio");
    std::this_thread::sleep_for(std::chrono::milliseconds(700));

    auto full_audio = capture.stop();
    std::cout << "Captured " << full_audio.size() << " samples.\n";

    std::cout << "Transcribing captured chunks...\n";
    std::string transcript;
    while (auto chunk = capture.chunks().pop()) {
        std::string text = engine.transcribe(*chunk);
        if (!text.empty()) {
            if (!transcript.empty()) transcript += " ";
            transcript += text;
        }
    }
    std::cout << "TRANSCRIPT: " << transcript << "\n";
    check(!transcript.empty(), "expected a non-empty transcript from captured monitor audio");

    std::cout << "Storing in a temporary SQLite DB...\n";
    auto tmp_dir = std::filesystem::temp_directory_path() / "callscribe_cpp_pipeline_test_db";
    std::filesystem::create_directories(tmp_dir);
    db::Database database((tmp_dir / "pipeline.db").string());
    db::Repository repo(database);

    int64_t customer_id = repo.get_or_create_customer("Pipeline Test Customer");
    int64_t call_id = repo.create_call(customer_id, "Pipeline Test Meeting", "Sarah");
    repo.append_segment(call_id, "Other", transcript);
    repo.finish_call(call_id, 5, "");

    std::string stored_transcript = repo.render_transcript(call_id);
    check(stored_transcript == "Other: " + transcript, "stored transcript did not round-trip correctly");

    std::cout << "Summarizing via local Ollama...\n";
    auto result = llm::summarize_call("http://localhost:11434", "qwen2.5:7b-instruct", stored_transcript,
                                       "Pipeline Test Customer", "Pipeline Test Meeting", "Sarah", "2026-07-28");
    repo.add_note(call_id, result.summary_text, result.email_draft, "[]");
    for (auto& t : result.tasks) repo.add_task(t, call_id, customer_id, "", "call");

    std::cout << "\nSUMMARY:\n" << result.summary_text << "\n";
    std::cout << "\nEMAIL:\n" << result.email_draft << "\n";
    std::cout << "\nTASKS: " << result.tasks.size() << "\n";
    for (auto& t : result.tasks) std::cout << "  - " << t << "\n";

    check(!result.summary_text.empty(), "expected a non-empty summary from Ollama");
    check(!result.email_draft.empty(), "expected a non-empty email draft from Ollama");

    auto note = repo.get_note_for_call(call_id);
    check(note.has_value() && note->summary == result.summary_text, "stored note did not round-trip correctly");
    auto tasks = repo.list_tasks("open");
    check(tasks.size() == result.tasks.size(), "stored task count did not match summarizer output");

    std::filesystem::remove(test_wav);
    std::filesystem::remove_all(tmp_dir);

    std::cout << "\nPIPELINE INTEGRATION TEST PASSED\n";
    return 0;
}
