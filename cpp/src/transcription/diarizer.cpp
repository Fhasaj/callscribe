#include "transcription/diarizer.hpp"

#include <sherpa-onnx/c-api/c-api.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <thread>

#include "config.hpp"
#include "util/download.hpp"

namespace callscribe::transcription {

namespace {

// Marks where each voice is active. pyannote segmentation-3.0, converted to
// ONNX by the sherpa-onnx author; ~6.6 MB.
constexpr const char* kSegmentationUrl =
    "https://huggingface.co/csukuangfj/sherpa-onnx-pyannote-segmentation-3-0/resolve/main/model.onnx";
constexpr const char* kSegmentationFile = "sherpa-pyannote-segmentation-3-0.onnx";

// Turns each active stretch into a speaker embedding. CAM++ trained on
// VoxCeleb (English) at ~28 MB -- the calls this app records are English, and
// the alternatives are either larger or trained on another language.
constexpr const char* kEmbeddingUrl =
    "https://github.com/k2-fsa/sherpa-onnx/releases/download/speaker-recongition-models/"
    "wespeaker_en_voxceleb_CAM++.onnx";
constexpr const char* kEmbeddingFile = "wespeaker_en_voxceleb_CAMPP.onnx";

// Used only when the speaker count is unknown. sherpa-onnx's own default;
// lower splits one person into several, higher merges two people into one.
constexpr float kClusterThreshold = 0.5f;

// Ignore a detected stretch shorter than this -- at conference-call quality,
// anything briefer is usually a cough or a cross-talk artefact rather than a
// turn worth attributing.
constexpr float kMinDurationOn = 0.3f;
// Bridge a gap this short rather than splitting one person's sentence in two
// across a breath.
constexpr float kMinDurationOff = 0.5f;

std::filesystem::path segmentation_model_path() { return models_dir() / kSegmentationFile; }
std::filesystem::path embedding_model_path() { return models_dir() / kEmbeddingFile; }

}  // namespace

std::vector<std::string> label_lines(const std::vector<DiarizedSegment>& speech,
                                      const std::vector<std::pair<int64_t, int64_t>>& lines,
                                      const std::vector<std::string>& participant_names) {
    std::vector<std::string> out(lines.size());
    if (speech.empty()) return out;

    // Renumber cluster ids by when each voice is first heard.
    std::vector<int> order;
    for (const DiarizedSegment& segment : speech) {
        if (std::find(order.begin(), order.end(), segment.speaker) == order.end())
            order.push_back(segment.speaker);
    }

    for (size_t i = 0; i < lines.size(); ++i) {
        const auto& [start_ms, end_ms] = lines[i];
        // The diarized stretch overlapping this line the most wins. A line can
        // straddle a handover, and whoever said more of it is the best single
        // answer available.
        int64_t best_overlap = 0;
        int best_speaker = -1;
        for (const DiarizedSegment& segment : speech) {
            int64_t overlap = std::min(end_ms, segment.end_ms) - std::max(start_ms, segment.start_ms);
            if (overlap > best_overlap) {
                best_overlap = overlap;
                best_speaker = segment.speaker;
            }
        }
        if (best_speaker < 0) continue;  // no speech here; leave the line alone

        size_t index = static_cast<size_t>(
            std::distance(order.begin(), std::find(order.begin(), order.end(), best_speaker)));
        out[i] = index < participant_names.size() ? participant_names[index]
                                                   : "Speaker " + std::to_string(index + 1);
    }
    return out;
}

std::string segmentation_model_filename() { return kSegmentationFile; }
std::string embedding_model_filename() { return kEmbeddingFile; }

bool diarization_models_present() {
    return std::filesystem::exists(segmentation_model_path()) &&
           std::filesystem::exists(embedding_model_path());
}

std::string diarization_models_size_note() { return "about 35 MB"; }

void ensure_diarization_models_downloaded(const ModelProgressCallback& on_progress) {
    // Only what is actually missing is fetched, and the count reflects that --
    // re-running after one file succeeded shows "1 of 1", not a phantom second.
    struct Pending {
        const char* url;
        std::filesystem::path path;
        const char* label;
    };
    std::vector<Pending> pending;
    if (!std::filesystem::exists(segmentation_model_path()))
        pending.push_back({kSegmentationUrl, segmentation_model_path(), "speaker segmentation"});
    if (!std::filesystem::exists(embedding_model_path()))
        pending.push_back({kEmbeddingUrl, embedding_model_path(), "speaker embeddings"});

    int count = static_cast<int>(pending.size());
    for (int i = 0; i < count; ++i) {
        const Pending& item = pending[static_cast<size_t>(i)];
        util::ProgressCallback forward;
        if (on_progress) {
            forward = [&on_progress, &item, i, count](int64_t downloaded, int64_t total) {
                on_progress(item.label, i + 1, count, downloaded, total);
            };
        }
        util::download_to_file(item.url, item.path, forward);
    }
}

Diarizer::Diarizer(const std::string& segmentation_model_path_in, const std::string& embedding_model_path_in,
                    int expected_speakers) {
    if (!std::filesystem::exists(segmentation_model_path_in) ||
        !std::filesystem::exists(embedding_model_path_in)) {
        throw std::runtime_error(
            "Speaker diarization models are missing -- download them from Settings.");
    }

    SherpaOnnxOfflineSpeakerDiarizationConfig config;
    std::memset(&config, 0, sizeof(config));

    // Diarization runs after the call, while nothing else is competing for the
    // machine, but Whisper's weights may still be resident -- half the cores is
    // plenty for two models this size and leaves the box usable.
    const int threads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency() / 2));

    config.segmentation.pyannote.model = segmentation_model_path_in.c_str();
    config.segmentation.num_threads = threads;
    config.segmentation.provider = "cpu";

    config.embedding.model = embedding_model_path_in.c_str();
    config.embedding.num_threads = threads;
    config.embedding.provider = "cpu";

    // Telling the clusterer how many voices to expect is the single biggest
    // quality lever here, and the participant list usually knows. Below two it
    // is not a usable hint -- one listed participant does not rule out someone
    // unlisted joining -- so the distance threshold decides instead.
    if (expected_speakers > 1) {
        config.clustering.num_clusters = expected_speakers;
    } else {
        config.clustering.num_clusters = -1;
        config.clustering.threshold = kClusterThreshold;
    }

    config.min_duration_on = kMinDurationOn;
    config.min_duration_off = kMinDurationOff;

    handle_ = SherpaOnnxCreateOfflineSpeakerDiarization(&config);
    if (!handle_) throw std::runtime_error("Failed to load the speaker diarization models");
}

Diarizer::~Diarizer() {
    if (handle_)
        SherpaOnnxDestroyOfflineSpeakerDiarization(
            static_cast<const SherpaOnnxOfflineSpeakerDiarization*>(handle_));
}

int Diarizer::sample_rate() const {
    if (!handle_) return 0;
    return SherpaOnnxOfflineSpeakerDiarizationGetSampleRate(
        static_cast<const SherpaOnnxOfflineSpeakerDiarization*>(handle_));
}

std::vector<DiarizedSegment> Diarizer::process(const std::vector<float>& audio) {
    if (!handle_ || audio.empty()) return {};
    const auto* sd = static_cast<const SherpaOnnxOfflineSpeakerDiarization*>(handle_);

    const SherpaOnnxOfflineSpeakerDiarizationResult* result =
        SherpaOnnxOfflineSpeakerDiarizationProcess(sd, audio.data(), static_cast<int32_t>(audio.size()));
    if (!result) return {};

    std::vector<DiarizedSegment> out;
    int32_t count = SherpaOnnxOfflineSpeakerDiarizationResultGetNumSegments(result);
    const SherpaOnnxOfflineSpeakerDiarizationSegment* segments =
        SherpaOnnxOfflineSpeakerDiarizationResultSortByStartTime(result);
    if (segments) {
        out.reserve(static_cast<size_t>(count));
        for (int32_t i = 0; i < count; ++i) {
            DiarizedSegment segment;
            segment.start_ms = static_cast<int64_t>(segments[i].start * 1000.0f);
            segment.end_ms = static_cast<int64_t>(segments[i].end * 1000.0f);
            segment.speaker = segments[i].speaker;
            out.push_back(segment);
        }
        SherpaOnnxOfflineSpeakerDiarizationDestroySegment(segments);
    }
    SherpaOnnxOfflineSpeakerDiarizationDestroyResult(result);
    return out;
}

}  // namespace callscribe::transcription
