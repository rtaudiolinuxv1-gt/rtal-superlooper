#include "RootTimingWorker.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <memory>
#include <numeric>

#include <aubio/aubio.h>

namespace {
constexpr uint_t kAubioWindowSize = 2048;
constexpr uint_t kAubioHopSize = 512;
constexpr size_t kChannels = 2;

struct AubioOnsetDeleter {
    void operator()(aubio_onset_t* ptr) const noexcept
    {
        if (ptr != nullptr) {
            del_aubio_onset(ptr);
        }
    }
};

struct AubioTempoDeleter {
    void operator()(aubio_tempo_t* ptr) const noexcept
    {
        if (ptr != nullptr) {
            del_aubio_tempo(ptr);
        }
    }
};

struct FvecDeleter {
    void operator()(fvec_t* ptr) const noexcept
    {
        if (ptr != nullptr) {
            del_fvec(ptr);
        }
    }
};

using AubioOnsetPtr = std::unique_ptr<aubio_onset_t, AubioOnsetDeleter>;
using AubioTempoPtr = std::unique_ptr<aubio_tempo_t, AubioTempoDeleter>;
using FvecPtr = std::unique_ptr<fvec_t, FvecDeleter>;

RootTimingResult makeError(const RootTimingRequest& request, const QString& errorMessage)
{
    RootTimingResult result;
    result.ok = false;
    result.errorMessage = errorMessage;
    result.sample = request.sample;
    result.sampleName = request.sampleName;
    result.sampleRate = request.sampleRate;
    result.frames = request.frames;
    result.barsPerRoot = request.barsPerRoot;
    result.requestId = request.requestId;
    return result;
}

std::vector<float> toMono(const std::vector<float>& stereoData, size_t frames)
{
    std::vector<float> mono(frames, 0.0F);
    for (size_t frame = 0; frame < frames; ++frame) {
        const size_t offset = frame * kChannels;
        mono[frame] = (stereoData[offset] + stereoData[offset + 1U]) * 0.5F;
    }
    return mono;
}

std::pair<double, double> detectActiveRegion(const std::vector<float>& mono, uint32_t sampleRate)
{
    if (mono.empty() || sampleRate == 0U) {
        return { 0.0, 0.0 };
    }

    float peak = 0.0F;
    for (float value : mono) {
        peak = std::max(peak, std::abs(value));
    }
    if (peak <= 0.000001F) {
        return { 0.0, static_cast<double>(mono.size()) };
    }

    const size_t windowFrames = std::max<size_t>(1U, static_cast<size_t>(sampleRate / 200U));
    const float threshold = std::max(peak * 0.08F, 0.0035F);

    auto energyAt = [&](size_t start) {
        const size_t end = std::min(mono.size(), start + windowFrames);
        if (end <= start) {
            return 0.0F;
        }
        double sumSquares = 0.0;
        for (size_t index = start; index < end; ++index) {
            const double sample = mono[index];
            sumSquares += sample * sample;
        }
        return static_cast<float>(std::sqrt(sumSquares / static_cast<double>(end - start)));
    };

    size_t startFrame = 0U;
    while (startFrame < mono.size() && energyAt(startFrame) < threshold) {
        ++startFrame;
    }

    size_t endFrame = mono.size();
    while (endFrame > startFrame + 1U && energyAt(endFrame - 1U) < threshold) {
        --endFrame;
    }

    return { static_cast<double>(startFrame), static_cast<double>(endFrame) };
}

std::vector<double> detectOnsetsWithAubio(
    const std::vector<float>& mono,
    uint32_t sampleRate,
    size_t startFrame = 0U,
    size_t endFrame = 0U)
{
    if (endFrame == 0U || endFrame > mono.size()) {
        endFrame = mono.size();
    }
    if (startFrame >= endFrame) {
        return {};
    }

    AubioOnsetPtr onset(new_aubio_onset(const_cast<char_t*>("default"), kAubioWindowSize, kAubioHopSize, sampleRate));
    if (!onset) {
        return {};
    }

    aubio_onset_set_silence(onset.get(), -70.0F);
    aubio_onset_set_threshold(onset.get(), 0.25F);

    FvecPtr input(new_fvec(kAubioHopSize));
    FvecPtr output(new_fvec(1));
    if (!input || !output) {
        return {};
    }

    std::vector<double> onsets;
    for (size_t offset = startFrame; offset < endFrame; offset += kAubioHopSize) {
        fvec_zeros(input.get());
        const size_t chunkFrames = std::min<size_t>(kAubioHopSize, endFrame - offset);
        for (size_t frame = 0; frame < chunkFrames; ++frame) {
            input->data[frame] = mono[offset + frame];
        }

        aubio_onset_do(onset.get(), input.get(), output.get());
        if (output->data[0] > 0.0F) {
            onsets.push_back(static_cast<double>(startFrame) + static_cast<double>(aubio_onset_get_last(onset.get())));
        }
    }

    return onsets;
}

struct TempoSummary {
    std::vector<double> beats;
    double bpm = 0.0;
    double confidence = 0.0;
    double periodFrames = 0.0;
};

TempoSummary detectTempoWithAubio(
    const std::vector<float>& mono,
    uint32_t sampleRate,
    size_t startFrame = 0U,
    size_t endFrame = 0U)
{
    if (endFrame == 0U || endFrame > mono.size()) {
        endFrame = mono.size();
    }
    if (startFrame >= endFrame) {
        return {};
    }

    AubioTempoPtr tempo(new_aubio_tempo(const_cast<char_t*>("default"), kAubioWindowSize, kAubioHopSize, sampleRate));
    if (!tempo) {
        return {};
    }

    aubio_tempo_set_silence(tempo.get(), -70.0F);
    aubio_tempo_set_threshold(tempo.get(), 0.25F);

    FvecPtr input(new_fvec(kAubioHopSize));
    FvecPtr output(new_fvec(1));
    if (!input || !output) {
        return {};
    }

    TempoSummary summary;
    for (size_t offset = startFrame; offset < endFrame; offset += kAubioHopSize) {
        fvec_zeros(input.get());
        const size_t chunkFrames = std::min<size_t>(kAubioHopSize, endFrame - offset);
        for (size_t frame = 0; frame < chunkFrames; ++frame) {
            input->data[frame] = mono[offset + frame];
        }

        aubio_tempo_do(tempo.get(), input.get(), output.get());
        if (output->data[0] > 0.0F) {
            summary.beats.push_back(static_cast<double>(startFrame) + static_cast<double>(aubio_tempo_get_last(tempo.get())));
        }
    }

    summary.bpm = static_cast<double>(aubio_tempo_get_bpm(tempo.get()));
    summary.confidence = static_cast<double>(aubio_tempo_get_confidence(tempo.get()));
    summary.periodFrames = static_cast<double>(aubio_tempo_get_period(tempo.get()));
    return summary;
}

struct AnalysisWindow {
    size_t startFrame = 0U;
    size_t endFrame = 0U;
};

std::vector<AnalysisWindow> buildAnalysisWindows(const RootTimingRequest& request)
{
    std::vector<AnalysisWindow> windows;
    if (request.frames == 0U || request.sampleRate == 0U) {
        return windows;
    }

    int scope = request.analysisScope;
    const size_t startWindowFrames = std::max<size_t>(
        1U,
        static_cast<size_t>(std::llround(std::max(1.0, request.analysisStartLengthSeconds) * static_cast<double>(request.sampleRate))));
    const size_t endWindowFrames = std::max<size_t>(
        1U,
        static_cast<size_t>(std::llround(std::max(1.0, request.analysisEndLengthSeconds) * static_cast<double>(request.sampleRate))));

    auto addWindow = [&windows](size_t startFrame, size_t endFrame) {
        if (startFrame < endFrame) {
            windows.push_back({ startFrame, endFrame });
        }
    };

    switch (scope) {
    case 0:
        addWindow(0U, std::min(request.frames, startWindowFrames));
        break;
    case 1:
        addWindow(request.frames > endWindowFrames ? request.frames - endWindowFrames : 0U, request.frames);
        break;
    case 2:
        addWindow(0U, std::min(request.frames, startWindowFrames));
        addWindow(request.frames > endWindowFrames ? request.frames - endWindowFrames : 0U, request.frames);
        break;
    case 3:
    default:
        addWindow(0U, request.frames);
        break;
    }

    return windows;
}

TempoSummary mergeTempoSummaries(const std::vector<TempoSummary>& summaries, uint32_t sampleRate, int mergePolicy)
{
    TempoSummary merged;
    std::vector<const TempoSummary*> valid;
    for (const TempoSummary& summary : summaries) {
        merged.beats.insert(merged.beats.end(), summary.beats.begin(), summary.beats.end());
        if (summary.bpm > 0.0 || summary.periodFrames > 0.0) {
            valid.push_back(&summary);
        }
    }
    std::sort(merged.beats.begin(), merged.beats.end());
    if (valid.empty()) {
        return merged;
    }
    if (valid.size() == 1U) {
        merged.bpm = valid.front()->bpm;
        merged.confidence = valid.front()->confidence;
        merged.periodFrames = valid.front()->periodFrames;
        return merged;
    }

    const TempoSummary* first = valid.front();
    const TempoSummary* second = valid.back();
    if (mergePolicy == 0) {
        if (first->confidence > second->confidence + 0.10) {
            merged.bpm = first->bpm;
            merged.confidence = first->confidence;
        } else if (second->confidence > first->confidence + 0.10) {
            merged.bpm = second->bpm;
            merged.confidence = second->confidence;
        } else {
            merged.bpm = (first->bpm + second->bpm) * 0.5;
            merged.confidence = std::max(first->confidence, second->confidence);
        }
    } else {
        double bpmSum = 0.0;
        double confidenceSum = 0.0;
        int bpmCount = 0;
        for (const TempoSummary* summary : valid) {
            if (summary->bpm > 0.0) {
                bpmSum += summary->bpm;
                ++bpmCount;
            }
            confidenceSum += summary->confidence;
        }
        merged.bpm = bpmCount > 0 ? bpmSum / static_cast<double>(bpmCount) : 0.0;
        merged.confidence = confidenceSum / static_cast<double>(valid.size());
    }

    if (merged.bpm > 0.0) {
        merged.periodFrames = (60.0 * static_cast<double>(sampleRate)) / merged.bpm;
    } else {
        double periodSum = 0.0;
        int periodCount = 0;
        for (const TempoSummary* summary : valid) {
            if (summary->periodFrames > 0.0) {
                periodSum += summary->periodFrames;
                ++periodCount;
            }
        }
        merged.periodFrames = periodCount > 0 ? periodSum / static_cast<double>(periodCount) : 0.0;
    }

    return merged;
}

double medianBeatPeriod(const std::vector<double>& beats)
{
    if (beats.size() < 2U) {
        return 0.0;
    }

    std::vector<double> deltas;
    deltas.reserve(beats.size() - 1U);
    for (size_t index = 1; index < beats.size(); ++index) {
        const double delta = beats[index] - beats[index - 1U];
        if (delta > 0.0) {
            deltas.push_back(delta);
        }
    }
    if (deltas.empty()) {
        return 0.0;
    }

    const size_t mid = deltas.size() / 2U;
    std::nth_element(deltas.begin(), deltas.begin() + static_cast<std::ptrdiff_t>(mid), deltas.end());
    return deltas[mid];
}

double nearestCandidate(double target, const std::vector<double>& candidates, double tolerance)
{
    double best = target;
    double bestDistance = tolerance;
    for (double candidate : candidates) {
        const double distance = std::abs(candidate - target);
        if (distance <= bestDistance) {
            best = candidate;
            bestDistance = distance;
        }
    }
    return best;
}
}

RootTimingWorker::RootTimingWorker(QObject* parent)
    : QObject(parent)
{
}

void RootTimingWorker::analyzeRootTiming(const RootTimingRequest& request)
{
    if (request.sample == nullptr) {
        emit analysisFinished(makeError(request, QStringLiteral("Missing sample.")));
        return;
    }

    if (request.frames == 0U || request.stereoData.size() < request.frames * kChannels || request.sampleRate == 0U) {
        emit analysisFinished(makeError(request, QStringLiteral("Invalid audio for beat analysis.")));
        return;
    }

    try {
        const std::vector<float> mono = toMono(request.stereoData, request.frames);
        const auto [activeStartFrame, activeEndFrame] = detectActiveRegion(mono, request.sampleRate);
        const std::vector<AnalysisWindow> windows = buildAnalysisWindows(request);
        std::vector<double> onsets;
        std::vector<TempoSummary> summaries;
        onsets.reserve(windows.size() * 8U);
        summaries.reserve(windows.size());
        for (const AnalysisWindow& window : windows) {
            std::vector<double> windowOnsets = detectOnsetsWithAubio(mono, request.sampleRate, window.startFrame, window.endFrame);
            onsets.insert(onsets.end(), windowOnsets.begin(), windowOnsets.end());
            summaries.push_back(detectTempoWithAubio(mono, request.sampleRate, window.startFrame, window.endFrame));
        }
        std::sort(onsets.begin(), onsets.end());
        TempoSummary tempo = mergeTempoSummaries(summaries, request.sampleRate, request.mergePolicy);

        std::vector<double> filteredOnsets;
        filteredOnsets.reserve(onsets.size());
        for (double onset : onsets) {
            if (onset >= activeStartFrame && onset <= activeEndFrame) {
                filteredOnsets.push_back(onset);
            }
        }

        std::vector<double> filteredBeats;
        filteredBeats.reserve(tempo.beats.size());
        for (double beat : tempo.beats) {
            if (beat >= activeStartFrame && beat <= activeEndFrame) {
                filteredBeats.push_back(beat);
            }
        }
        tempo.beats = std::move(filteredBeats);

        if (tempo.periodFrames <= 0.0) {
            tempo.periodFrames = medianBeatPeriod(tempo.beats);
        }
        if (tempo.periodFrames <= 0.0 && tempo.bpm > 0.0) {
            tempo.periodFrames = (60.0 * static_cast<double>(request.sampleRate)) / tempo.bpm;
        }
        if (tempo.bpm <= 0.0 && tempo.periodFrames > 0.0) {
            tempo.bpm = (60.0 * static_cast<double>(request.sampleRate)) / tempo.periodFrames;
        }

        double suggestedStartFrame = !filteredOnsets.empty()
            ? filteredOnsets.front()
            : activeStartFrame;
        if (tempo.periodFrames > 0.0 && !tempo.beats.empty()) {
            suggestedStartFrame = nearestCandidate(
                suggestedStartFrame,
                tempo.beats,
                tempo.periodFrames * 0.30);
        }

        double suggestedEndFrame = activeEndFrame;
        const double targetBeatCount = static_cast<double>(std::max(1, request.barsPerRoot) * 4);
        if (tempo.periodFrames > 0.0) {
            const double desiredEndFrame = suggestedStartFrame + (tempo.periodFrames * targetBeatCount);
            std::vector<double> endCandidates = tempo.beats;
            endCandidates.insert(endCandidates.end(), filteredOnsets.begin(), filteredOnsets.end());
            endCandidates.push_back(activeEndFrame);
            suggestedEndFrame = nearestCandidate(desiredEndFrame, endCandidates, tempo.periodFrames * 0.40);
            if (suggestedEndFrame <= suggestedStartFrame) {
                suggestedEndFrame = desiredEndFrame;
            }
        }

        suggestedStartFrame = std::clamp(suggestedStartFrame, 0.0, static_cast<double>(request.frames));
        suggestedEndFrame = std::clamp(suggestedEndFrame, suggestedStartFrame, static_cast<double>(request.frames));

        RootTimingResult result;
        result.ok = true;
        result.sample = request.sample;
        result.sampleName = request.sampleName;
        result.sampleRate = request.sampleRate;
        result.frames = request.frames;
        result.barsPerRoot = request.barsPerRoot;
        result.requestId = request.requestId;
        result.usedAubio = true;
        result.hasBeatGrid = tempo.periodFrames > 0.0 && tempo.beats.size() >= 2U;
        result.suggestedStartFrame = suggestedStartFrame;
        result.suggestedEndFrame = suggestedEndFrame;
        result.beatPeriodFrames = tempo.periodFrames;
        result.tempoBpm = tempo.bpm;
        result.confidence = tempo.confidence;
        result.beatCount = static_cast<int>(tempo.beats.size());
        result.onsetCount = static_cast<int>(filteredOnsets.size());
        result.message = result.hasBeatGrid
            ? QStringLiteral("Analyzed Root timing: %1 BPM, %2 beats detected")
                .arg(result.tempoBpm, 0, 'f', 1)
                .arg(result.beatCount)
            : QStringLiteral("Root silence/onset analysis complete; no stable beat grid found");
        emit analysisFinished(result);
    } catch (const std::exception& error) {
        emit analysisFinished(makeError(request, QString::fromLocal8Bit(error.what())));
    } catch (...) {
        emit analysisFinished(makeError(request, QStringLiteral("Unknown root timing analysis error.")));
    }
}
