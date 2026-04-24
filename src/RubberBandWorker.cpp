#include "RubberBandWorker.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>

#include <rubberband/RubberBandStretcher.h>

namespace {
constexpr size_t kChannelCount = 2;

StretchResult makeError(const StretchRequest& request, const QString& message)
{
    StretchResult result;
    result.ok = false;
    result.errorMessage = message;
    result.sample = request.sample;
    result.sampleName = request.sampleName;
    result.sampleRate = request.sampleRate;
    result.targetDurationSec = request.targetDurationSec;
    result.requestId = request.requestId;
    return result;
}

void deinterleave(const std::vector<float>& interleaved, size_t frames, std::vector<float>* left, std::vector<float>* right)
{
    left->assign(frames, 0.0F);
    right->assign(frames, 0.0F);
    for (size_t frame = 0; frame < frames; ++frame) {
        const size_t offset = frame * kChannelCount;
        (*left)[frame] = interleaved[offset];
        (*right)[frame] = interleaved[offset + 1U];
    }
}

std::vector<float> interleaveExactLength(
    const std::vector<float>& left,
    const std::vector<float>& right,
    size_t availableFrames,
    size_t targetFrames)
{
    std::vector<float> result(targetFrames * kChannelCount, 0.0F);
    const size_t framesToCopy = std::min(availableFrames, targetFrames);
    for (size_t frame = 0; frame < framesToCopy; ++frame) {
        const size_t offset = frame * kChannelCount;
        result[offset] = left[frame];
        result[offset + 1U] = right[frame];
    }
    return result;
}
}

RubberBandWorker::RubberBandWorker(QObject* parent)
    : QObject(parent)
{
}

void RubberBandWorker::stretchToDuration(const StretchRequest& request)
{
    if (request.sample == nullptr) {
        emit stretchFinished(makeError(request, QStringLiteral("Missing sample.")));
        return;
    }

    if (request.frames == 0U || request.stereoData.size() < request.frames * kChannelCount || request.sampleRate == 0U) {
        emit stretchFinished(makeError(request, QStringLiteral("Invalid input audio.")));
        return;
    }

    if (request.targetDurationSec <= 0.0) {
        emit stretchFinished(makeError(request, QStringLiteral("Invalid target duration.")));
        return;
    }

    const size_t targetFrames = std::max<size_t>(
        1U,
        static_cast<size_t>(std::llround(request.targetDurationSec * static_cast<double>(request.sampleRate))));
    const double ratio = static_cast<double>(targetFrames) / static_cast<double>(request.frames);

    try {
        std::vector<float> left;
        std::vector<float> right;
        deinterleave(request.stereoData, request.frames, &left, &right);

        RubberBand::RubberBandStretcher stretcher(
            request.sampleRate,
            kChannelCount,
            RubberBand::RubberBandStretcher::OptionProcessOffline
                | RubberBand::RubberBandStretcher::OptionStretchPrecise
                | RubberBand::RubberBandStretcher::OptionTransientsCrisp
                | RubberBand::RubberBandStretcher::OptionDetectorCompound
                | RubberBand::RubberBandStretcher::OptionPhaseLaminar
                | RubberBand::RubberBandStretcher::OptionThreadingNever,
            ratio,
            1.0);
        stretcher.setExpectedInputDuration(request.frames);
        const size_t maxChunkFrames = std::min<size_t>(65536U, stretcher.getProcessSizeLimit());

        for (size_t offset = 0; offset < request.frames; offset += maxChunkFrames) {
            const size_t chunkFrames = std::min(maxChunkFrames, request.frames - offset);
            std::array<const float*, kChannelCount> studyInput {
                left.data() + offset,
                right.data() + offset,
            };
            const bool finalChunk = (offset + chunkFrames) >= request.frames;
            stretcher.study(studyInput.data(), chunkFrames, finalChunk);
        }

        std::vector<float> outLeft;
        std::vector<float> outRight;
        outLeft.reserve(targetFrames + 4096U);
        outRight.reserve(targetFrames + 4096U);

        auto retrieveAvailable = [&stretcher, &outLeft, &outRight]() {
            while (true) {
                const int available = stretcher.available();
                if (available <= 0) {
                    break;
                }

                std::vector<float> chunkLeft(static_cast<size_t>(available), 0.0F);
                std::vector<float> chunkRight(static_cast<size_t>(available), 0.0F);
                std::array<float*, kChannelCount> output {
                    chunkLeft.data(),
                    chunkRight.data(),
                };
                const size_t retrieved = stretcher.retrieve(output.data(), static_cast<size_t>(available));
                if (retrieved == 0U) {
                    break;
                }

                outLeft.insert(outLeft.end(), chunkLeft.cbegin(), chunkLeft.cbegin() + static_cast<std::ptrdiff_t>(retrieved));
                outRight.insert(outRight.end(), chunkRight.cbegin(), chunkRight.cbegin() + static_cast<std::ptrdiff_t>(retrieved));
            }
        };

        for (size_t offset = 0; offset < request.frames; offset += maxChunkFrames) {
            const size_t chunkFrames = std::min(maxChunkFrames, request.frames - offset);
            std::array<const float*, kChannelCount> processInput {
                left.data() + offset,
                right.data() + offset,
            };
            const bool finalChunk = (offset + chunkFrames) >= request.frames;
            stretcher.process(processInput.data(), chunkFrames, finalChunk);
            retrieveAvailable();
        }

        retrieveAvailable();

        StretchResult result;
        result.ok = true;
        result.sample = request.sample;
        result.sampleName = request.sampleName;
        result.stereoData = interleaveExactLength(outLeft, outRight, outLeft.size(), targetFrames);
        result.sampleRate = request.sampleRate;
        result.frames = targetFrames;
        result.targetDurationSec = request.targetDurationSec;
        result.requestId = request.requestId;
        emit stretchFinished(result);
    } catch (const std::exception& error) {
        emit stretchFinished(makeError(request, QString::fromLocal8Bit(error.what())));
    } catch (...) {
        emit stretchFinished(makeError(request, QStringLiteral("Unknown RubberBand error.")));
    }
}
