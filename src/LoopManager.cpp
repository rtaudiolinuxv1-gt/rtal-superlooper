#include "LoopManager.h"

#include "Lv2Host.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {
constexpr double kDefaultMaxRecordSeconds = 60.0;
constexpr float kAutoStartThreshold = 0.012F;
constexpr float kAutoStopThreshold = 0.006F;
constexpr double kAutoSilenceSeconds = 0.25;
constexpr double kAutoMinimumSeconds = 0.08;
constexpr size_t kEnvelopeCurveResolution = 1025;

constexpr std::array<float, kEnvelopeCurveResolution> makeEnvelopeCurve() noexcept
{
    std::array<float, kEnvelopeCurveResolution> values {};
    for (size_t index = 0; index < values.size(); ++index) {
        values[index] = static_cast<float>(index) / static_cast<float>(values.size() - 1U);
    }
    return values;
}

constexpr std::array<float, kEnvelopeCurveResolution> kEnvelopeCurve = makeEnvelopeCurve();

bool validNoteIndex(int noteIndex)
{
    return noteIndex >= 0 && noteIndex < PianoWidget::kKeyCount;
}

size_t framesForDuration(double seconds, uint32_t sampleRate)
{
    const double clampedSeconds = std::clamp(seconds, 0.001, kDefaultMaxRecordSeconds);
    return static_cast<size_t>(std::ceil(clampedSeconds * static_cast<double>(sampleRate)));
}

size_t framesForShortDuration(double seconds, uint32_t sampleRate)
{
    if (seconds <= 0.0 || sampleRate == 0U) {
        return 0U;
    }

    return static_cast<size_t>(std::ceil(std::clamp(seconds, 0.0, 0.250) * static_cast<double>(sampleRate)));
}

std::pair<size_t, size_t> playableFrameRange(
    const Sample& sample,
    double trimStartNormalized = 0.0,
    double trimEndNormalized = 1.0)
{
    const size_t sampleStart = static_cast<size_t>(
        std::clamp(sample.startFrame, 0.0, 1.0) * static_cast<double>(sample.frames));
    const size_t sampleEnd = static_cast<size_t>(
        std::clamp(sample.endFrame, 0.0, 1.0) * static_cast<double>(sample.frames));
    const size_t keyStart = static_cast<size_t>(
        std::clamp(trimStartNormalized, 0.0, 1.0) * static_cast<double>(sample.frames));
    const size_t keyEnd = static_cast<size_t>(
        std::clamp(trimEndNormalized, 0.0, 1.0) * static_cast<double>(sample.frames));
    const size_t clampedStart = std::min(std::max(sampleStart, keyStart), sample.frames);
    const size_t clampedEnd = std::min(std::max(clampedStart, std::min(sampleEnd, std::max(keyEnd, clampedStart))), sample.frames);
    return { clampedStart, clampedEnd };
}

float panLeftGain(float pan) noexcept
{
    return pan > 0.0F ? 1.0F - std::clamp(pan, 0.0F, 1.0F) : 1.0F;
}

float panRightGain(float pan) noexcept
{
    return pan < 0.0F ? 1.0F + std::clamp(pan, -1.0F, 0.0F) : 1.0F;
}

float softLimit(float value) noexcept
{
    if (std::abs(value) <= 1.0F) {
        return value;
    }

    return std::tanh(value);
}

float interpolatedSampleValue(const std::vector<float>& data, size_t baseOffset, size_t nextOffset, double fraction) noexcept
{
    return static_cast<float>((static_cast<double>(data[baseOffset]) * (1.0 - fraction))
        + (static_cast<double>(data[nextOffset]) * fraction));
}

float lookupEnvelopeCurve(size_t numerator, size_t denominator) noexcept
{
    if (denominator == 0U) {
        return 1.0F;
    }

    const double normalized = std::clamp(
        static_cast<double>(numerator) / static_cast<double>(denominator),
        0.0,
        1.0);
    const size_t index = static_cast<size_t>(std::llround(normalized * static_cast<double>(kEnvelopeCurveResolution - 1U)));
    return kEnvelopeCurve[std::min(index, kEnvelopeCurveResolution - 1U)];
}

}

LoopManager::LoopManager()
{
    for (KeySlot& slot : keySlots_) {
        for (size_t index = 0; index < kLv2ChainSize; ++index) {
            slot.plugins[index].store(nullptr, std::memory_order_release);
            slot.pluginEnabled[index].store(true, std::memory_order_release);
        }
    }

    for (size_t index = 0; index < kLv2ChainSize; ++index) {
        masterPlugins_[index].store(nullptr, std::memory_order_release);
        masterPluginEnabled_[index].store(true, std::memory_order_release);
    }
}

void LoopManager::setSampleRate(uint32_t sampleRate) noexcept
{
    if (sampleRate == 0U) {
        return;
    }

    sampleRate_.store(sampleRate, std::memory_order_release);
    refreshTimingCaches();
}

uint32_t LoopManager::sampleRate() const noexcept
{
    return sampleRate_.load(std::memory_order_acquire);
}

void LoopManager::assignSample(int noteIndex, const Sample* sample) noexcept
{
    if (!validNoteIndex(noteIndex)) {
        return;
    }

    keySlots_[static_cast<size_t>(noteIndex)].sample.store(sample, std::memory_order_release);
}

void LoopManager::setKeyVolume(int noteIndex, float volume) noexcept
{
    if (!validNoteIndex(noteIndex)) {
        return;
    }

    const auto permille = static_cast<uint32_t>(std::llround(std::clamp(volume, 0.0F, 2.0F) * 1000.0F));
    keySlots_[static_cast<size_t>(noteIndex)].volumePermille.store(permille, std::memory_order_release);
}

void LoopManager::setKeyPan(int noteIndex, float pan) noexcept
{
    if (!validNoteIndex(noteIndex)) {
        return;
    }

    const auto permille = static_cast<int32_t>(std::llround(std::clamp(pan, -1.0F, 1.0F) * 1000.0F));
    keySlots_[static_cast<size_t>(noteIndex)].panPermille.store(permille, std::memory_order_release);
}

void LoopManager::setKeyMuted(int noteIndex, bool muted) noexcept
{
    if (validNoteIndex(noteIndex)) {
        keySlots_[static_cast<size_t>(noteIndex)].muted.store(muted, std::memory_order_release);
    }
}

void LoopManager::setKeySolo(int noteIndex, bool solo) noexcept
{
    if (validNoteIndex(noteIndex)) {
        keySlots_[static_cast<size_t>(noteIndex)].solo.store(solo, std::memory_order_release);
    }
}

void LoopManager::setKeyGroup(int noteIndex, int groupIndex) noexcept
{
    if (!validNoteIndex(noteIndex)) {
        return;
    }

    keySlots_[static_cast<size_t>(noteIndex)].groupIndex.store(
        std::clamp(groupIndex, 0, static_cast<int>(kGroupCount) - 1),
        std::memory_order_release);
}

void LoopManager::setKeyAttackMs(int noteIndex, double milliseconds) noexcept
{
    if (!validNoteIndex(noteIndex)) {
        return;
    }

    const auto microseconds = static_cast<uint32_t>(std::llround(std::clamp(milliseconds, 0.0, 5000.0) * 1000.0));
    KeySlot& slot = keySlots_[static_cast<size_t>(noteIndex)];
    slot.attackTimeUs.store(microseconds, std::memory_order_release);
    refreshKeyTimingCache(&slot);
}

void LoopManager::setKeyReleaseMs(int noteIndex, double milliseconds) noexcept
{
    if (!validNoteIndex(noteIndex)) {
        return;
    }

    const auto microseconds = static_cast<uint32_t>(std::llround(std::clamp(milliseconds, 0.0, 5000.0) * 1000.0));
    KeySlot& slot = keySlots_[static_cast<size_t>(noteIndex)];
    slot.releaseTimeUs.store(microseconds, std::memory_order_release);
    refreshKeyTimingCache(&slot);
}

void LoopManager::setKeyStaccato(int noteIndex, float amount) noexcept
{
    if (!validNoteIndex(noteIndex)) {
        return;
    }

    const auto permille = static_cast<uint32_t>(std::llround(std::clamp(amount, 0.0F, 1.0F) * 1000.0F));
    KeySlot& slot = keySlots_[static_cast<size_t>(noteIndex)];
    slot.staccatoPermille.store(permille, std::memory_order_release);
    refreshKeyTimingCache(&slot);
}

void LoopManager::setKeyLoopMode(int noteIndex, KeyLoopMode mode) noexcept
{
    if (!validNoteIndex(noteIndex)) {
        return;
    }

    keySlots_[static_cast<size_t>(noteIndex)].loopEnabled.store(mode == KeyLoopMode::Loop, std::memory_order_release);
}

void LoopManager::setKeyVirtualStaccatoEnabled(int noteIndex, bool enabled) noexcept
{
    if (!validNoteIndex(noteIndex)) {
        return;
    }

    keySlots_[static_cast<size_t>(noteIndex)].virtualStaccatoEnabled.store(enabled, std::memory_order_release);
}

void LoopManager::setKeySelfMixEnabled(int noteIndex, bool enabled) noexcept
{
    if (!validNoteIndex(noteIndex)) {
        return;
    }

    keySlots_[static_cast<size_t>(noteIndex)].selfMixEnabled.store(enabled, std::memory_order_release);
}

void LoopManager::setKeyTrimRange(int noteIndex, double startNormalized, double endNormalized) noexcept
{
    if (!validNoteIndex(noteIndex)) {
        return;
    }

    const double clampedStart = std::clamp(startNormalized, 0.0, 1.0);
    const double clampedEnd = std::clamp(endNormalized, clampedStart, 1.0);
    keySlots_[static_cast<size_t>(noteIndex)].trimStartPermille.store(
        static_cast<uint32_t>(std::llround(clampedStart * 1000000.0)),
        std::memory_order_release);
    keySlots_[static_cast<size_t>(noteIndex)].trimEndPermille.store(
        static_cast<uint32_t>(std::llround(clampedEnd * 1000000.0)),
        std::memory_order_release);
}

void LoopManager::setKeyPlaybackRate(int noteIndex, double playbackRate) noexcept
{
    if (!validNoteIndex(noteIndex)) {
        return;
    }

    const double clampedRate = std::clamp(playbackRate, 0.0625, 16.0);
    keySlots_[static_cast<size_t>(noteIndex)].playbackRatePermille.store(
        static_cast<uint32_t>(std::llround(clampedRate * 1000.0)),
        std::memory_order_release);
}

void LoopManager::setKeyLv2Processor(int noteIndex, size_t slotIndex, Lv2Processor* processor) noexcept
{
    if (!validNoteIndex(noteIndex) || slotIndex >= kLv2ChainSize) {
        return;
    }

    keySlots_[static_cast<size_t>(noteIndex)].plugins[slotIndex].store(processor, std::memory_order_release);
}

void LoopManager::setKeyLv2Enabled(int noteIndex, size_t slotIndex, bool enabled) noexcept
{
    if (!validNoteIndex(noteIndex) || slotIndex >= kLv2ChainSize) {
        return;
    }

    keySlots_[static_cast<size_t>(noteIndex)].pluginEnabled[slotIndex].store(enabled, std::memory_order_release);
}

void LoopManager::setMasterGain(float gain) noexcept
{
    const auto permille = static_cast<uint32_t>(std::llround(std::clamp(gain, 0.0F, 2.0F) * 1000.0F));
    masterGainPermille_.store(permille, std::memory_order_release);
}

void LoopManager::setLimiterEnabled(bool enabled) noexcept
{
    limiterEnabled_.store(enabled, std::memory_order_release);
}

void LoopManager::setSampleNormalizationEnabled(bool enabled) noexcept
{
    sampleNormalizationEnabled_.store(enabled, std::memory_order_release);
}

void LoopManager::setFadeTimeMs(double milliseconds) noexcept
{
    const auto microseconds = static_cast<uint32_t>(std::llround(std::clamp(milliseconds, 0.0, 60.0) * 1000.0));
    fadeTimeUs_.store(microseconds, std::memory_order_release);
    fadeFramesCached_.store(
        framesForShortDuration(static_cast<double>(microseconds) / 1000000.0, sampleRate()),
        std::memory_order_release);
}

void LoopManager::setLoopCrossfade(bool enabled, double milliseconds) noexcept
{
    loopCrossfadeEnabled_.store(enabled, std::memory_order_release);
    const auto microseconds = static_cast<uint32_t>(std::llround(std::clamp(milliseconds, 1.0, 60.0) * 1000.0));
    loopCrossfadeTimeUs_.store(microseconds, std::memory_order_release);
    loopCrossfadeFramesCached_.store(
        framesForShortDuration(static_cast<double>(microseconds) / 1000000.0, sampleRate()),
        std::memory_order_release);
}

void LoopManager::setGroupGain(int groupIndex, float gain) noexcept
{
    if (groupIndex < 0 || groupIndex >= static_cast<int>(kGroupCount)) {
        return;
    }

    const auto permille = static_cast<uint32_t>(std::llround(std::clamp(gain, 0.0F, 2.0F) * 1000.0F));
    groupSlots_[static_cast<size_t>(groupIndex)].gainPermille.store(permille, std::memory_order_release);
}

void LoopManager::setGroupPan(int groupIndex, float pan) noexcept
{
    if (groupIndex < 0 || groupIndex >= static_cast<int>(kGroupCount)) {
        return;
    }

    const auto permille = static_cast<int32_t>(std::llround(std::clamp(pan, -1.0F, 1.0F) * 1000.0F));
    groupSlots_[static_cast<size_t>(groupIndex)].panPermille.store(permille, std::memory_order_release);
}

void LoopManager::setGroupMuted(int groupIndex, bool muted) noexcept
{
    if (groupIndex >= 0 && groupIndex < static_cast<int>(kGroupCount)) {
        groupSlots_[static_cast<size_t>(groupIndex)].muted.store(muted, std::memory_order_release);
    }
}

void LoopManager::setGroupSolo(int groupIndex, bool solo) noexcept
{
    if (groupIndex >= 0 && groupIndex < static_cast<int>(kGroupCount)) {
        groupSlots_[static_cast<size_t>(groupIndex)].solo.store(solo, std::memory_order_release);
    }
}

void LoopManager::setMasterLv2Processor(size_t slotIndex, Lv2Processor* processor) noexcept
{
    if (slotIndex >= kLv2ChainSize) {
        return;
    }

    masterPlugins_[slotIndex].store(processor, std::memory_order_release);
}

void LoopManager::setMasterLv2Enabled(size_t slotIndex, bool enabled) noexcept
{
    if (slotIndex >= kLv2ChainSize) {
        return;
    }

    masterPluginEnabled_[slotIndex].store(enabled, std::memory_order_release);
}

float LoopManager::outputPeakLeft() const noexcept
{
    return static_cast<float>(outputPeakLeftPermille_.load(std::memory_order_acquire)) / 1000.0F;
}

float LoopManager::outputPeakRight() const noexcept
{
    return static_cast<float>(outputPeakRightPermille_.load(std::memory_order_acquire)) / 1000.0F;
}

bool LoopManager::limiterWasActive() const noexcept
{
    return limiterActive_.load(std::memory_order_acquire);
}

void LoopManager::noteOn(int noteIndex) noexcept
{
    if (!validNoteIndex(noteIndex)) {
        return;
    }

    keySlots_[static_cast<size_t>(noteIndex)].noteOnRequested.store(true, std::memory_order_release);
}

void LoopManager::noteOff(int noteIndex) noexcept
{
    if (!validNoteIndex(noteIndex)) {
        return;
    }

    keySlots_[static_cast<size_t>(noteIndex)].noteOffRequested.store(true, std::memory_order_release);
}

void LoopManager::togglePlayback(int noteIndex) noexcept
{
    if (!validNoteIndex(noteIndex)) {
        return;
    }

    keySlots_[static_cast<size_t>(noteIndex)].togglePlaybackRequested.store(true, std::memory_order_release);
}

void LoopManager::togglePausePlayback(int noteIndex) noexcept
{
    if (!validNoteIndex(noteIndex)) {
        return;
    }

    keySlots_[static_cast<size_t>(noteIndex)].togglePauseRequested.store(true, std::memory_order_release);
}

void LoopManager::startRecording(int noteIndex, double maxDurationSeconds, bool autoMode)
{
    if (!validNoteIndex(noteIndex)) {
        return;
    }

    KeySlot& slot = keySlots_[static_cast<size_t>(noteIndex)];
    if (slot.recordingState.load(std::memory_order_acquire) == static_cast<int>(RecordingState::Recording)) {
        return;
    }

    const uint32_t currentSampleRate = sampleRate();
    const size_t capacityFrames = framesForDuration(maxDurationSeconds, currentSampleRate);
    slot.recordBuffer.assign(capacityFrames * 2U, 0.0F);
    slot.recordCapacityFrames = capacityFrames;
    slot.recordedFrames = 0;
    slot.autoMode = autoMode;
    slot.autoStarted = !autoMode;
    slot.autoSilentFrames = 0;
    slot.stopRecordingRequested.store(false, std::memory_order_release);
    slot.targetFrames.store(capacityFrames, std::memory_order_release);
    slot.recordingState.store(static_cast<int>(RecordingState::Recording), std::memory_order_release);
}

void LoopManager::setRecordingTarget(int noteIndex, double durationSeconds) noexcept
{
    if (!validNoteIndex(noteIndex)) {
        return;
    }

    const uint32_t currentSampleRate = sampleRate();
    const size_t target = framesForDuration(durationSeconds, currentSampleRate);
    keySlots_[static_cast<size_t>(noteIndex)].targetFrames.store(target, std::memory_order_release);
}

void LoopManager::stopRecording(int noteIndex) noexcept
{
    if (!validNoteIndex(noteIndex)) {
        return;
    }

    keySlots_[static_cast<size_t>(noteIndex)].stopRecordingRequested.store(true, std::memory_order_release);
}

void LoopManager::playLayered(const Sample* first, const Sample* second) noexcept
{
    layerSlots_[0].sample.store(first, std::memory_order_release);
    layerSlots_[1].sample.store(second, std::memory_order_release);
    layeredToggleRequested_.store(true, std::memory_order_release);
}

void LoopManager::detachSample(const Sample* sample) noexcept
{
    if (sample == nullptr) {
        return;
    }

    for (KeySlot& slot : keySlots_) {
        const Sample* expected = sample;
        slot.sample.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel, std::memory_order_acquire);
    }

    for (LayerSlot& slot : layerSlots_) {
        const Sample* expected = sample;
        slot.sample.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel, std::memory_order_acquire);
    }

    const Sample* expected = sample;
    previewSlot_.sample.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel, std::memory_order_acquire);
}

void LoopManager::startPreview(const Sample* sample, size_t startFrame, int direction, double speedRatio) noexcept
{
    if (sample == nullptr || sample->frames == 0U) {
        return;
    }

    const int normalizedDirection = direction < 0 ? -1 : 1;
    const double clampedSpeed = std::clamp(speedRatio, 0.125, 8.0);
    previewSlot_.sample.store(sample, std::memory_order_release);
    previewSlot_.startFrame.store(std::min(startFrame, sample->frames - 1U), std::memory_order_release);
    previewSlot_.direction.store(normalizedDirection, std::memory_order_release);
    previewSlot_.speedPermille.store(static_cast<uint32_t>(std::llround(clampedSpeed * 1000.0)), std::memory_order_release);
    previewSlot_.stopRequested.store(false, std::memory_order_release);
    previewSlot_.startRequested.store(true, std::memory_order_release);
}

void LoopManager::stopPreview() noexcept
{
    previewSlot_.stopRequested.store(true, std::memory_order_release);
}

void LoopManager::stopAllPlayback() noexcept
{
    stopAllRequested_.store(true, std::memory_order_release);
}

bool LoopManager::takeFinishedRecording(FinishedRecording* recording)
{
    if (recording == nullptr) {
        return false;
    }

    for (int noteIndex = 0; noteIndex < PianoWidget::kKeyCount; ++noteIndex) {
        KeySlot& slot = keySlots_[static_cast<size_t>(noteIndex)];
        int expected = static_cast<int>(RecordingState::Finished);
        if (!slot.recordingState.compare_exchange_strong(
                expected,
                static_cast<int>(RecordingState::Idle),
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            continue;
        }

        const size_t frames = std::min(slot.recordedFrames, slot.recordCapacityFrames);
        recording->noteIndex = noteIndex;
        recording->sampleRate = sampleRate();
        recording->frames = frames;
        recording->data.assign(slot.recordBuffer.cbegin(), slot.recordBuffer.cbegin() + static_cast<std::ptrdiff_t>(frames * 2U));
        return true;
    }

    return false;
}

void LoopManager::process(
    jack_nframes_t frameCount,
    const jack_default_audio_sample_t* inputLeft,
    const jack_default_audio_sample_t* inputRight,
    jack_default_audio_sample_t* outputLeft,
    jack_default_audio_sample_t* outputRight) noexcept
{
    if (outputLeft != nullptr) {
        std::fill_n(outputLeft, frameCount, 0.0F);
    }
    if (outputRight != nullptr) {
        std::fill_n(outputRight, frameCount, 0.0F);
    }

    processRecordings(frameCount, inputLeft, inputRight);
    const bool stoppedAllPlayback = processStopAllRequest();

    if (outputLeft == nullptr || outputRight == nullptr) {
        return;
    }

    if (stoppedAllPlayback) {
        return;
    }

    const bool anyKeySolo = anyKeySoloActive();
    for (jack_nframes_t offset = 0; offset < frameCount; offset += kMaxMixBlockFrames) {
        const jack_nframes_t blockFrames = std::min<jack_nframes_t>(kMaxMixBlockFrames, frameCount - offset);
        clearMixBuffers(blockFrames);
        processKeyPlayback(blockFrames, offset, anyKeySolo);
        processLayeredPlayback(blockFrames, offset);
        processPreviewPlayback(blockFrames, offset);
        finalizeMix(blockFrames, outputLeft + offset, outputRight + offset);
    }
}

void LoopManager::mixSample(
    const Sample& sample,
        double* playbackFrame,
        bool* isPlaying,
        bool* stopQueued,
        size_t* fadeInFramesRemaining,
        size_t* fadeOutFramesRemaining,
        bool* pauseAfterFade,
        double trimStartNormalized,
        double trimEndNormalized,
        double playbackRate,
        jack_nframes_t frameCount,
        jack_default_audio_sample_t* outputLeft,
        jack_default_audio_sample_t* outputRight,
    float gain,
    float pan,
    bool sampleNormalizationEnabled,
    size_t attackFrames,
    size_t releaseFrames,
    bool crossfadeEnabled,
    size_t crossfadeFrames,
    bool* keyHeld,
    size_t* postReleaseFramesRemaining,
    size_t stopAfterFrames) noexcept
{
    if (sample.frames == 0U || sample.data.size() < sample.frames * 2U) {
        *isPlaying = false;
        *stopQueued = false;
        *playbackFrame = 0;
        return;
    }

    const auto [rangeStart, rangeEnd] = playableFrameRange(sample, trimStartNormalized, trimEndNormalized);
    if (rangeEnd <= rangeStart) {
        *isPlaying = false;
        *stopQueued = false;
        *playbackFrame = 0.0;
        return;
    }

    const size_t rangeLength = rangeEnd - rangeStart;
    const size_t effectiveCrossfadeFrames = crossfadeEnabled
        ? std::min(crossfadeFrames, rangeLength / 2U)
        : 0U;
    const float panLeft = panLeftGain(pan);
    const float panRight = panRightGain(pan);
    const float sampleGain = sample.gain * (sampleNormalizationEnabled ? sample.normalizationGain : 1.0F);
    double frame = std::clamp(*playbackFrame, 0.0, std::max(0.0, static_cast<double>(rangeLength) - 1.0));
    const double effectiveRate = std::max(0.0625, playbackRate);
    const size_t clampedAttackFrames = std::max<size_t>(1U, attackFrames);
    const size_t clampedReleaseFrames = std::max<size_t>(1U, releaseFrames);
    const size_t releaseTailFrames = releaseFrames > 0U ? std::min(releaseFrames, rangeLength) : 0U;
    for (jack_nframes_t outputFrame = 0; outputFrame < frameCount; ++outputFrame) {
        const size_t frameIndex = std::min(static_cast<size_t>(frame), rangeLength - 1U);
        const double fraction = std::clamp(frame - static_cast<double>(frameIndex), 0.0, 1.0);
        const size_t nextFrameIndex = std::min(frameIndex + 1U, rangeLength - 1U);
        const size_t offset = (rangeStart + frameIndex) * 2U;
        const size_t nextOffset = (rangeStart + nextFrameIndex) * 2U;
        float left = interpolatedSampleValue(sample.data, offset, nextOffset, fraction);
        float right = interpolatedSampleValue(sample.data, offset + 1U, nextOffset + 1U, fraction);

        if (effectiveCrossfadeFrames > 0U && !*stopQueued) {
            const double framesUntilEnd = static_cast<double>(rangeLength) - frame;
            if (framesUntilEnd <= effectiveCrossfadeFrames) {
                const float blend = 1.0F - (static_cast<float>(framesUntilEnd) / static_cast<float>(effectiveCrossfadeFrames));
                const size_t loopOffset = rangeStart * 2U;
                left = (left * (1.0F - blend)) + (sample.data[loopOffset] * blend);
                right = (right * (1.0F - blend)) + (sample.data[loopOffset + 1U] * blend);
            }
        }

        float fadeGain = 1.0F;
        if (keyHeld != nullptr && postReleaseFramesRemaining != nullptr && !*keyHeld && *isPlaying) {
            if (*postReleaseFramesRemaining > 0U) {
                --(*postReleaseFramesRemaining);
            } else if (*fadeOutFramesRemaining == 0U) {
                if (releaseFrames > 0U) {
                    *fadeOutFramesRemaining = releaseFrames;
                } else {
                    *isPlaying = false;
                    *stopQueued = false;
                    *pauseAfterFade = false;
                }
            }
        }
        if (attackFrames > 0U && *fadeInFramesRemaining > 0U) {
            const size_t fadePosition = attackFrames - std::min(*fadeInFramesRemaining, attackFrames);
            fadeGain *= lookupEnvelopeCurve(fadePosition, clampedAttackFrames);
            --(*fadeInFramesRemaining);
        }
        if (releaseFrames > 0U && *fadeOutFramesRemaining > 0U) {
            fadeGain *= lookupEnvelopeCurve(std::min(*fadeOutFramesRemaining, releaseFrames), clampedReleaseFrames);
            --(*fadeOutFramesRemaining);
            if (*fadeOutFramesRemaining == 0U) {
                const bool shouldPause = *pauseAfterFade;
                *isPlaying = false;
                *stopQueued = false;
                if (!shouldPause) {
                    frame = 0;
                }
                *pauseAfterFade = false;
                if (keyHeld != nullptr) {
                    *keyHeld = false;
                }
                if (postReleaseFramesRemaining != nullptr) {
                    *postReleaseFramesRemaining = 0U;
                }
            }
        }
        if (releaseFrames > 0U && *stopQueued && frame >= rangeLength - releaseTailFrames) {
            const size_t framesUntilEnd = rangeLength - frame;
            fadeGain *= lookupEnvelopeCurve(std::min(framesUntilEnd, releaseTailFrames), std::max<size_t>(1U, releaseTailFrames));
        }

        outputLeft[outputFrame] += left * gain * sampleGain * fadeGain * panLeft;
        outputRight[outputFrame] += right * gain * sampleGain * fadeGain * panRight;

        if (!*isPlaying) {
            break;
        }

        frame += effectiveRate;
        if (stopAfterFrames > 0U && frame >= static_cast<double>(stopAfterFrames)) {
            *isPlaying = false;
            *stopQueued = false;
            *fadeInFramesRemaining = 0;
            *fadeOutFramesRemaining = 0;
            *pauseAfterFade = false;
            if (keyHeld != nullptr) {
                *keyHeld = false;
            }
            if (postReleaseFramesRemaining != nullptr) {
                *postReleaseFramesRemaining = 0U;
            }
            break;
        }
        while (frame >= static_cast<double>(rangeLength)) {
            frame -= static_cast<double>(rangeLength);
            if (*stopQueued) {
                *isPlaying = false;
                *stopQueued = false;
                *fadeInFramesRemaining = 0;
                *fadeOutFramesRemaining = 0;
                *pauseAfterFade = false;
                if (keyHeld != nullptr) {
                    *keyHeld = false;
                }
                if (postReleaseFramesRemaining != nullptr) {
                    *postReleaseFramesRemaining = 0U;
                }
                break;
            }
        }
        if (!*isPlaying) {
            break;
        }
    }

    *playbackFrame = frame;
}

void LoopManager::processRecordings(
    jack_nframes_t frameCount,
    const jack_default_audio_sample_t* inputLeft,
    const jack_default_audio_sample_t* inputRight) noexcept
{
    const uint32_t currentSampleRate = sampleRate();
    const size_t autoSilenceFrames = framesForDuration(kAutoSilenceSeconds, currentSampleRate);
    const size_t autoMinimumFrames = framesForDuration(kAutoMinimumSeconds, currentSampleRate);

    for (KeySlot& slot : keySlots_) {
        if (slot.recordingState.load(std::memory_order_acquire) != static_cast<int>(RecordingState::Recording)) {
            continue;
        }

        bool finishRecording = slot.stopRecordingRequested.exchange(false, std::memory_order_acq_rel);
        size_t writeFrame = slot.recordedFrames;
        const size_t targetFrames = std::min(slot.targetFrames.load(std::memory_order_acquire), slot.recordCapacityFrames);

        for (jack_nframes_t inputFrame = 0; inputFrame < frameCount && !finishRecording; ++inputFrame) {
            if (writeFrame >= targetFrames || writeFrame >= slot.recordCapacityFrames) {
                finishRecording = true;
                break;
            }

            const float left = inputLeft != nullptr ? inputLeft[inputFrame] : 0.0F;
            const float right = inputRight != nullptr ? inputRight[inputFrame] : left;
            const float peak = std::max(std::abs(left), std::abs(right));

            if (slot.autoMode) {
                if (!slot.autoStarted) {
                    if (peak < kAutoStartThreshold) {
                        continue;
                    }
                    slot.autoStarted = true;
                }

                if (peak < kAutoStopThreshold) {
                    ++slot.autoSilentFrames;
                } else {
                    slot.autoSilentFrames = 0;
                }

                if (writeFrame >= autoMinimumFrames && slot.autoSilentFrames >= autoSilenceFrames) {
                    const size_t writtenSilentFrames = slot.autoSilentFrames > 0U ? slot.autoSilentFrames - 1U : 0U;
                    writeFrame -= std::min(writeFrame, writtenSilentFrames);
                    finishRecording = true;
                    break;
                }
            }

            const size_t outputOffset = writeFrame * 2U;
            slot.recordBuffer[outputOffset] = left;
            slot.recordBuffer[outputOffset + 1U] = right;
            ++writeFrame;
        }

        slot.recordedFrames = writeFrame;
        if (finishRecording || writeFrame >= targetFrames || writeFrame >= slot.recordCapacityFrames) {
            slot.recordingState.store(static_cast<int>(RecordingState::Finished), std::memory_order_release);
        }
    }
}

void LoopManager::processKeyPlayback(
    jack_nframes_t frameCount,
    size_t outputOffset,
    bool anyKeySolo) noexcept
{
    (void)outputOffset;
    const size_t globalFadeFrames = fadeFramesCached_.load(std::memory_order_acquire);
    const bool crossfadeEnabled = loopCrossfadeEnabled_.load(std::memory_order_acquire);
    const size_t crossfadeFrames = loopCrossfadeFramesCached_.load(std::memory_order_acquire);
    const bool sampleNormalizationEnabled = sampleNormalizationEnabled_.load(std::memory_order_acquire);
    auto resetVoice = [globalFadeFrames](PlaybackVoice* voice, bool stopQueued, bool keyHeld, bool releaseTrackingEnabled) {
        if (voice == nullptr) {
            return;
        }
        voice->playbackFrame = 0.0;
        voice->stopQueued = stopQueued;
        voice->fadeInFramesRemaining = globalFadeFrames;
        voice->fadeOutFramesRemaining = 0U;
        voice->pauseAfterFade = false;
        voice->releaseTrackingEnabled = releaseTrackingEnabled;
        voice->keyHeld = keyHeld;
        voice->postReleaseFramesRemaining = 0U;
        voice->isPlaying = true;
    };
    auto releaseHeldVoice = [](KeySlot* slot, size_t postReleaseFrames) {
        if (slot == nullptr) {
            return;
        }

        auto releaseVoice = [postReleaseFrames](PlaybackVoice* voice) {
            if (voice == nullptr || !voice->isPlaying || !voice->releaseTrackingEnabled || !voice->keyHeld) {
                return false;
            }
            voice->keyHeld = false;
            voice->postReleaseFramesRemaining = postReleaseFrames;
            return true;
        };

        if (releaseVoice(&slot->primaryVoice)) {
            return;
        }
        for (PlaybackVoice& voice : slot->overlapVoices) {
            if (releaseVoice(&voice)) {
                return;
            }
        }
    };

    for (KeySlot& slot : keySlots_) {
        const bool loopEnabled = slot.loopEnabled.load(std::memory_order_acquire);
        const bool selfMixEnabled = slot.selfMixEnabled.load(std::memory_order_acquire);
        const bool virtualStaccatoEnabled = slot.virtualStaccatoEnabled.load(std::memory_order_acquire);
        const size_t staccatoFrames = slot.staccatoFramesCached.load(std::memory_order_acquire);

        if (slot.noteOnRequested.exchange(false, std::memory_order_acq_rel)) {
            if (!loopEnabled && slot.sample.load(std::memory_order_acquire) != nullptr) {
                PlaybackVoice* voice = !slot.primaryVoice.isPlaying
                    ? &slot.primaryVoice
                    : (selfMixEnabled ? choosePlaybackVoiceForRetrigger(&slot) : &slot.primaryVoice);
                resetVoice(voice, true, true, virtualStaccatoEnabled);
            }
        }

        if (slot.noteOffRequested.exchange(false, std::memory_order_acq_rel)) {
            if (!loopEnabled && virtualStaccatoEnabled) {
                releaseHeldVoice(&slot, staccatoFrames);
            }
        }

        if (slot.togglePauseRequested.exchange(false, std::memory_order_acq_rel)) {
            if (slot.primaryVoice.isPlaying) {
                slot.primaryVoice.stopQueued = false;
                if (globalFadeFrames == 0U) {
                    slot.primaryVoice.isPlaying = false;
                } else {
                    slot.primaryVoice.fadeOutFramesRemaining = globalFadeFrames;
                    slot.primaryVoice.pauseAfterFade = true;
                }
            } else if (slot.sample.load(std::memory_order_acquire) != nullptr) {
                slot.primaryVoice.stopQueued = false;
                slot.primaryVoice.fadeInFramesRemaining = globalFadeFrames;
                slot.primaryVoice.fadeOutFramesRemaining = 0U;
                slot.primaryVoice.pauseAfterFade = false;
                slot.primaryVoice.isPlaying = true;
            }
        }

        if (slot.togglePlaybackRequested.exchange(false, std::memory_order_acq_rel)) {
            if (!loopEnabled && selfMixEnabled && slot.sample.load(std::memory_order_acquire) != nullptr) {
                PlaybackVoice* voice = !slot.primaryVoice.isPlaying
                    ? &slot.primaryVoice
                    : choosePlaybackVoiceForRetrigger(&slot);
                resetVoice(voice, true, false, false);
            } else if (slot.primaryVoice.isPlaying) {
                slot.primaryVoice.stopQueued = true;
            } else if (slot.sample.load(std::memory_order_acquire) != nullptr) {
                resetVoice(&slot.primaryVoice, !loopEnabled, false, false);
            }
        }

        const bool anyVoicePlaying = slot.primaryVoice.isPlaying
            || std::any_of(slot.overlapVoices.cbegin(), slot.overlapVoices.cend(), [](const PlaybackVoice& voice) {
                   return voice.isPlaying;
               });
        if (!anyVoicePlaying) {
            continue;
        }

        if (slot.muted.load(std::memory_order_acquire)
            || (anyKeySolo && !slot.solo.load(std::memory_order_acquire))) {
            continue;
        }

        const Sample* sample = slot.sample.load(std::memory_order_acquire);
        if (sample == nullptr) {
            slot.primaryVoice = PlaybackVoice();
            slot.overlapVoices.fill(PlaybackVoice());
            continue;
        }

        const float gain = static_cast<float>(slot.volumePermille.load(std::memory_order_acquire)) / 1000.0F;
        const float pan = static_cast<float>(slot.panPermille.load(std::memory_order_acquire)) / 1000.0F;
        const int groupIndex = std::clamp(
            slot.groupIndex.load(std::memory_order_acquire),
            0,
            static_cast<int>(kGroupCount) - 1);
        const size_t attackFrames = std::max(globalFadeFrames, slot.attackFramesCached.load(std::memory_order_acquire));
        const size_t releaseFrames = std::max(globalFadeFrames, slot.releaseFramesCached.load(std::memory_order_acquire));
        const double trimStart = static_cast<double>(slot.trimStartPermille.load(std::memory_order_acquire)) / 1000000.0;
        const double trimEnd = static_cast<double>(slot.trimEndPermille.load(std::memory_order_acquire)) / 1000000.0;
        const double playbackRate = static_cast<double>(
            std::max<uint32_t>(1U, slot.playbackRatePermille.load(std::memory_order_acquire))) / 1000.0;
        const size_t stopAfterFrames = 0U;
        std::fill_n(workLeftBuffer_.data(), frameCount, 0.0F);
        std::fill_n(workRightBuffer_.data(), frameCount, 0.0F);
        auto processVoice = [&](PlaybackVoice* voice) {
            if (voice == nullptr || !voice->isPlaying) {
                return;
            }

            mixSample(
                *sample,
                &voice->playbackFrame,
                &voice->isPlaying,
                &voice->stopQueued,
                &voice->fadeInFramesRemaining,
                &voice->fadeOutFramesRemaining,
                &voice->pauseAfterFade,
                trimStart,
                trimEnd,
                playbackRate,
                frameCount,
                workLeftBuffer_.data(),
                workRightBuffer_.data(),
                gain,
                pan,
                sampleNormalizationEnabled,
                attackFrames,
                releaseFrames,
                crossfadeEnabled,
                crossfadeFrames,
                voice->releaseTrackingEnabled ? &voice->keyHeld : nullptr,
                voice->releaseTrackingEnabled ? &voice->postReleaseFramesRemaining : nullptr,
                stopAfterFrames);
        };
        processVoice(&slot.primaryVoice);
        for (PlaybackVoice& voice : slot.overlapVoices) {
            processVoice(&voice);
        }
        for (size_t pluginIndex = 0; pluginIndex < kLv2ChainSize; ++pluginIndex) {
            if (!slot.pluginEnabled[pluginIndex].load(std::memory_order_acquire)) {
                continue;
            }
            if (Lv2Processor* processor = slot.plugins[pluginIndex].load(std::memory_order_acquire); processor != nullptr) {
                processor->process(workLeftBuffer_.data(), workRightBuffer_.data(), frameCount);
            }
        }
        for (jack_nframes_t frame = 0; frame < frameCount; ++frame) {
            groupLeftBuffers_[static_cast<size_t>(groupIndex)][static_cast<size_t>(frame)] += workLeftBuffer_[static_cast<size_t>(frame)];
            groupRightBuffers_[static_cast<size_t>(groupIndex)][static_cast<size_t>(frame)] += workRightBuffer_[static_cast<size_t>(frame)];
        }
    }
}

void LoopManager::processLayeredPlayback(
    jack_nframes_t frameCount,
    size_t outputOffset) noexcept
{
    (void)outputOffset;
    if (layeredToggleRequested_.exchange(false, std::memory_order_acq_rel)) {
        const bool anyPlaying = layerSlots_[0].isPlaying || layerSlots_[1].isPlaying;
        for (LayerSlot& slot : layerSlots_) {
            if (anyPlaying) {
                slot.stopQueued = true;
                continue;
            }

            if (slot.sample.load(std::memory_order_acquire) != nullptr) {
                slot.playbackFrame = 0.0;
                slot.stopQueued = false;
                slot.fadeInFramesRemaining = framesForShortDuration(
                    static_cast<double>(fadeTimeUs_.load(std::memory_order_acquire)) / 1000000.0,
                    sampleRate());
                slot.isPlaying = true;
            }
        }
    }

    const size_t fadeFrames = fadeFramesCached_.load(std::memory_order_acquire);
    const bool crossfadeEnabled = loopCrossfadeEnabled_.load(std::memory_order_acquire);
    const size_t crossfadeFrames = loopCrossfadeFramesCached_.load(std::memory_order_acquire);
    const bool sampleNormalizationEnabled = sampleNormalizationEnabled_.load(std::memory_order_acquire);

    for (LayerSlot& slot : layerSlots_) {
        if (!slot.isPlaying) {
            continue;
        }

        const Sample* sample = slot.sample.load(std::memory_order_acquire);
        if (sample == nullptr) {
            slot.isPlaying = false;
            slot.stopQueued = false;
            slot.playbackFrame = 0.0;
            continue;
        }

        size_t fadeOut = 0;
        bool pauseAfterFade = false;
        mixSample(
            *sample,
            &slot.playbackFrame,
            &slot.isPlaying,
            &slot.stopQueued,
            &slot.fadeInFramesRemaining,
            &fadeOut,
            &pauseAfterFade,
            0.0,
            1.0,
            1.0,
            frameCount,
            groupLeftBuffers_[0].data(),
            groupRightBuffers_[0].data(),
            1.0F,
            0.0F,
            sampleNormalizationEnabled,
            fadeFrames,
            fadeFrames,
            crossfadeEnabled,
            crossfadeFrames,
            nullptr,
            nullptr,
            0U);
    }
}

void LoopManager::processPreviewPlayback(
    jack_nframes_t frameCount,
    size_t outputOffset) noexcept
{
    (void)outputOffset;
    if (previewSlot_.stopRequested.exchange(false, std::memory_order_acq_rel)) {
        previewSlot_.isPlaying = false;
        previewSlot_.activeSample = nullptr;
    }

    if (previewSlot_.startRequested.exchange(false, std::memory_order_acq_rel)) {
        previewSlot_.activeSample = previewSlot_.sample.load(std::memory_order_acquire);
        if (previewSlot_.activeSample != nullptr && previewSlot_.activeSample->frames > 0U) {
            previewSlot_.framePosition = static_cast<double>(
                std::min(previewSlot_.startFrame.load(std::memory_order_acquire), previewSlot_.activeSample->frames - 1U));
            previewSlot_.activeDirection = previewSlot_.direction.load(std::memory_order_acquire) < 0 ? -1 : 1;
            previewSlot_.activeSpeed = static_cast<double>(
                std::max<uint32_t>(1U, previewSlot_.speedPermille.load(std::memory_order_acquire))) / 1000.0;
            previewSlot_.isPlaying = true;
        }
    }

    if (!previewSlot_.isPlaying || previewSlot_.activeSample == nullptr) {
        return;
    }

    const Sample& sample = *previewSlot_.activeSample;
    if (sample.frames == 0U || sample.data.size() < sample.frames * 2U) {
        previewSlot_.isPlaying = false;
        previewSlot_.activeSample = nullptr;
        return;
    }

    double framePosition = previewSlot_.framePosition;
    const double increment = previewSlot_.activeSpeed * static_cast<double>(previewSlot_.activeDirection);
    for (jack_nframes_t outputFrame = 0; outputFrame < frameCount; ++outputFrame) {
        if (framePosition < 0.0 || framePosition >= static_cast<double>(sample.frames)) {
            previewSlot_.isPlaying = false;
            previewSlot_.activeSample = nullptr;
            break;
        }

        const auto frame = static_cast<size_t>(framePosition);
        const size_t offset = frame * 2U;
        groupLeftBuffers_[0][static_cast<size_t>(outputFrame)] += sample.data[offset];
        groupRightBuffers_[0][static_cast<size_t>(outputFrame)] += sample.data[offset + 1U];
        framePosition += increment;
    }

    previewSlot_.framePosition = framePosition;
}

void LoopManager::clearMixBuffers(jack_nframes_t frameCount) noexcept
{
    for (size_t group = 0; group < kGroupCount; ++group) {
        std::fill_n(groupLeftBuffers_[group].data(), frameCount, 0.0F);
        std::fill_n(groupRightBuffers_[group].data(), frameCount, 0.0F);
    }
    std::fill_n(workLeftBuffer_.data(), frameCount, 0.0F);
    std::fill_n(workRightBuffer_.data(), frameCount, 0.0F);
    std::fill_n(masterLeftBuffer_.data(), frameCount, 0.0F);
    std::fill_n(masterRightBuffer_.data(), frameCount, 0.0F);
}

void LoopManager::finalizeMix(
    jack_nframes_t frameCount,
    jack_default_audio_sample_t* outputLeft,
    jack_default_audio_sample_t* outputRight) noexcept
{
    const bool anyGroupSolo = anyGroupSoloActive();
    const float masterGain = static_cast<float>(masterGainPermille_.load(std::memory_order_acquire)) / 1000.0F;
    const bool limiterEnabled = limiterEnabled_.load(std::memory_order_acquire);
    std::array<bool, kGroupCount> groupEnabled {};
    std::array<float, kGroupCount> groupGain {};
    std::array<float, kGroupCount> groupLeftPan {};
    std::array<float, kGroupCount> groupRightPan {};
    for (size_t group = 0; group < kGroupCount; ++group) {
        const GroupSlot& slot = groupSlots_[group];
        groupEnabled[group] = !slot.muted.load(std::memory_order_acquire)
            && (!anyGroupSolo || slot.solo.load(std::memory_order_acquire));
        groupGain[group] = static_cast<float>(slot.gainPermille.load(std::memory_order_acquire)) / 1000.0F;
        const float groupPan = static_cast<float>(slot.panPermille.load(std::memory_order_acquire)) / 1000.0F;
        groupLeftPan[group] = panLeftGain(groupPan);
        groupRightPan[group] = panRightGain(groupPan);
    }
    float peakLeft = 0.0F;
    float peakRight = 0.0F;
    bool limited = false;

    for (jack_nframes_t frame = 0; frame < frameCount; ++frame) {
        float left = 0.0F;
        float right = 0.0F;
        for (size_t group = 0; group < kGroupCount; ++group) {
            if (!groupEnabled[group]) {
                continue;
            }

            left += groupLeftBuffers_[group][static_cast<size_t>(frame)] * groupGain[group] * groupLeftPan[group];
            right += groupRightBuffers_[group][static_cast<size_t>(frame)] * groupGain[group] * groupRightPan[group];
        }
        masterLeftBuffer_[static_cast<size_t>(frame)] = left;
        masterRightBuffer_[static_cast<size_t>(frame)] = right;
    }

    for (size_t pluginIndex = 0; pluginIndex < kLv2ChainSize; ++pluginIndex) {
        if (!masterPluginEnabled_[pluginIndex].load(std::memory_order_acquire)) {
            continue;
        }
        if (Lv2Processor* processor = masterPlugins_[pluginIndex].load(std::memory_order_acquire); processor != nullptr) {
            processor->process(masterLeftBuffer_.data(), masterRightBuffer_.data(), frameCount);
        }
    }

    for (jack_nframes_t frame = 0; frame < frameCount; ++frame) {
        float left = masterLeftBuffer_[static_cast<size_t>(frame)] * masterGain;
        float right = masterRightBuffer_[static_cast<size_t>(frame)] * masterGain;

        if (limiterEnabled) {
            const float limitedLeft = softLimit(left);
            const float limitedRight = softLimit(right);
            limited = limited || std::abs(limitedLeft - left) > 0.000001F || std::abs(limitedRight - right) > 0.000001F;
            left = limitedLeft;
            right = limitedRight;
        }

        outputLeft[frame] += left;
        outputRight[frame] += right;
        peakLeft = std::max(peakLeft, std::abs(left));
        peakRight = std::max(peakRight, std::abs(right));
    }

    outputPeakLeftPermille_.store(static_cast<uint32_t>(std::llround(std::min(peakLeft, 9.999F) * 1000.0F)), std::memory_order_release);
    outputPeakRightPermille_.store(static_cast<uint32_t>(std::llround(std::min(peakRight, 9.999F) * 1000.0F)), std::memory_order_release);
    limiterActive_.store(limited, std::memory_order_release);
}

bool LoopManager::anyKeySoloActive() const noexcept
{
    return std::any_of(keySlots_.cbegin(), keySlots_.cend(), [](const KeySlot& slot) {
        return slot.solo.load(std::memory_order_acquire);
    });
}

bool LoopManager::anyGroupSoloActive() const noexcept
{
    return std::any_of(groupSlots_.cbegin(), groupSlots_.cend(), [](const GroupSlot& slot) {
        return slot.solo.load(std::memory_order_acquire);
    });
}

bool LoopManager::processStopAllRequest() noexcept
{
    if (!stopAllRequested_.exchange(false, std::memory_order_acq_rel)) {
        return false;
    }

    for (KeySlot& slot : keySlots_) {
        slot.noteOnRequested.store(false, std::memory_order_release);
        slot.noteOffRequested.store(false, std::memory_order_release);
        slot.togglePlaybackRequested.store(false, std::memory_order_release);
        slot.togglePauseRequested.store(false, std::memory_order_release);
        slot.primaryVoice = PlaybackVoice();
        slot.overlapVoices.fill(PlaybackVoice());
    }

    layeredToggleRequested_.store(false, std::memory_order_release);
    for (LayerSlot& slot : layerSlots_) {
        slot.isPlaying = false;
        slot.stopQueued = false;
        slot.playbackFrame = 0.0;
        slot.fadeInFramesRemaining = 0;
    }

    previewSlot_.startRequested.store(false, std::memory_order_release);
    previewSlot_.stopRequested.store(false, std::memory_order_release);
    previewSlot_.activeSample = nullptr;
    previewSlot_.framePosition = 0.0;
    previewSlot_.isPlaying = false;

    return true;
}

void LoopManager::refreshTimingCaches() noexcept
{
    fadeFramesCached_.store(
        framesForShortDuration(
            static_cast<double>(fadeTimeUs_.load(std::memory_order_acquire)) / 1000000.0,
            sampleRate()),
        std::memory_order_release);
    loopCrossfadeFramesCached_.store(
        framesForShortDuration(
            static_cast<double>(loopCrossfadeTimeUs_.load(std::memory_order_acquire)) / 1000000.0,
            sampleRate()),
        std::memory_order_release);

    for (KeySlot& slot : keySlots_) {
        refreshKeyTimingCache(&slot);
    }
}

void LoopManager::refreshKeyTimingCache(KeySlot* slot) noexcept
{
    if (slot == nullptr) {
        return;
    }

    const uint32_t currentSampleRate = sampleRate();
    slot->attackFramesCached.store(
        framesForShortDuration(
            static_cast<double>(slot->attackTimeUs.load(std::memory_order_acquire)) / 1000000.0,
            currentSampleRate),
        std::memory_order_release);
    slot->releaseFramesCached.store(
        framesForShortDuration(
            static_cast<double>(slot->releaseTimeUs.load(std::memory_order_acquire)) / 1000000.0,
            currentSampleRate),
        std::memory_order_release);
    const double staccatoPercent = static_cast<double>(slot->staccatoPermille.load(std::memory_order_acquire)) / 1000.0;
    const double postReleaseTailMs = 150.0 - (staccatoPercent * 100.0);
    slot->staccatoFramesCached.store(
        framesForShortDuration(postReleaseTailMs / 1000.0, currentSampleRate),
        std::memory_order_release);
}

LoopManager::PlaybackVoice* LoopManager::choosePlaybackVoiceForRetrigger(KeySlot* slot) noexcept
{
    if (slot == nullptr) {
        return nullptr;
    }

    for (PlaybackVoice& voice : slot->overlapVoices) {
        if (!voice.isPlaying) {
            return &voice;
        }
    }

    auto it = std::max_element(
        slot->overlapVoices.begin(),
        slot->overlapVoices.end(),
        [](const PlaybackVoice& left, const PlaybackVoice& right) {
            return left.playbackFrame < right.playbackFrame;
        });
    return it != slot->overlapVoices.end() ? &(*it) : &slot->primaryVoice;
}
