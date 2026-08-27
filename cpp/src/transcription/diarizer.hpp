#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "util/download.hpp"

namespace callscribe::transcription {

// Offline speaker diarization over the far end of a call, via sherpa-onnx.
//
// This is the "who is speaking" half of speaker labelling, and it is a signal
// problem, not a language one: a pyannote segmentation model marks where a
// voice is active, a speaker-embedding model turns each stretch into a vector,
// and the vectors are clustered so the same voice gets the same id wherever it
// occurs. Whisper cannot do this (it transcribes words, not voices), and
// neither can an LLM downstream -- by the time there is text, the voice is gone.
//
// Deliberately offline rather than streaming. It runs once, after the call, on
// the whole far-end recording: clustering that can see the entire conversation
// before deciding how many speakers there were is markedly more accurate than
// one guessing incrementally. The live transcript is labelled best-effort while
// recording, and corrected here when the call ends.
//
// Everything is local -- two ONNX files on disk and no network at inference
// time, which is the point.

// One stretch of audio attributed to one clustered speaker.
struct DiarizedSegment {
    int64_t start_ms = 0;
    int64_t end_ms = 0;
    int speaker = 0;  // cluster id, 0-based; stable across the whole result
};

// Assigns a speaker name to each transcript line from diarized speech.
//
// `lines` are the [start_ms, end_ms) ranges of the transcript lines to label,
// in any order. The returned vector is parallel to it; an entry is empty when
// no diarized speech overlaps that line at all, meaning the caller should keep
// whatever label the line already has rather than invent one.
//
// Cluster ids are renumbered by first appearance, so the first voice heard
// takes the first participant name -- the only ordering a user can predict.
// Voices beyond the supplied names become "Speaker 2", "Speaker 3", and so on,
// because a numbered speaker is honest where a guessed name is not.
//
// Pure and free-standing so the matching rules can be tested without models.
std::vector<std::string> label_lines(const std::vector<DiarizedSegment>& speech,
                                      const std::vector<std::pair<int64_t, int64_t>>& lines,
                                      const std::vector<std::string>& participant_names);

// Filenames under models_dir(). Kept next to the Whisper weights.
std::string segmentation_model_filename();
std::string embedding_model_filename();
bool diarization_models_present();

// Reports which of the two files is being fetched and how far along it is.
// `label` names the current file, `index`/`count` place it in the sequence.
using ModelProgressCallback =
    std::function<void(const std::string& label, int index, int count, int64_t downloaded, int64_t total)>;

// Downloads whichever of the two models is missing. Blocking -- call from a
// worker thread. Throws std::runtime_error on failure.
void ensure_diarization_models_downloaded(const ModelProgressCallback& on_progress = nullptr);

// Combined size of the two downloads, for a Settings message worth showing
// before someone starts a ~35 MB fetch.
std::string diarization_models_size_note();

class Diarizer {
public:
    // `expected_speakers` is the number of people on the far end, when known.
    // Clustering is much steadier when it is told how many voices to find, and
    // the participant list on the Record screen is exactly that number. Pass
    // anything below 2 to let it work the count out for itself.
    //
    // Throws std::runtime_error when the models are missing or won't load.
    Diarizer(const std::string& segmentation_model_path, const std::string& embedding_model_path,
              int expected_speakers);
    ~Diarizer();
    Diarizer(const Diarizer&) = delete;
    Diarizer& operator=(const Diarizer&) = delete;

    // `audio` is mono float32 in [-1, 1]. Must be at the rate reported by
    // sample_rate() -- which is 16 kHz for these models, matching capture.
    // Returns segments sorted by start time; empty when nothing was found.
    std::vector<DiarizedSegment> process(const std::vector<float>& audio);

    int sample_rate() const;

private:
    // Opaque sherpa-onnx handle; the C API type is kept out of this header so
    // the rest of the app doesn't need sherpa's includes.
    const void* handle_ = nullptr;
};

}  // namespace callscribe::transcription
