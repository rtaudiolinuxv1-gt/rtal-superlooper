#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <jack/types.h>

#include "PianoWidget.h"
#include "Sample.h"

class Lv2Processor;

/**
 * Completed recording payload copied out on the GUI thread.
 */
struct FinishedRecording {
    int noteIndex = -1;
    std::vector<float> data;
    uint32_t sampleRate = 48000;
    size_t frames = 0;
};

/**
 * Real-time loop state for 88 keys.
 *
 * The JACK callback calls process() only. GUI methods prepare storage and post
 * atomic requests; they do not directly mutate callback-local playback fields.
 */
class LoopManager final
{
public:
    static constexpr size_t kLv2ChainSize = 5;

    LoopManager();

    void setSampleRate(uint32_t sampleRate) noexcept;
    [[nodiscard]] uint32_t sampleRate() const noexcept;

    void assignSample(int noteIndex, const Sample* sample) noexcept;
    void setKeyVolume(int noteIndex, float volume) noexcept;
    void setKeyPan(int noteIndex, float pan) noexcept;
    void setKeyMuted(int noteIndex, bool muted) noexcept;
    void setKeySolo(int noteIndex, bool solo) noexcept;
    void setKeyGroup(int noteIndex, int groupIndex) noexcept;
    void setKeyAttackMs(int noteIndex, double milliseconds) noexcept;
    void setKeyReleaseMs(int noteIndex, double milliseconds) noexcept;
    void setKeyStaccato(int noteIndex, float amount) noexcept;
    void setKeyLoopMode(int noteIndex, KeyLoopMode mode) noexcept;
    void setKeyVirtualStaccatoEnabled(int noteIndex, bool enabled) noexcept;
    void setKeySelfMixEnabled(int noteIndex, bool enabled) noexcept;
    void setKeyTrimRange(int noteIndex, double startNormalized, double endNormalized) noexcept;
    void setKeyPlaybackRate(int noteIndex, double playbackRate) noexcept;
    void setKeyLv2Processor(int noteIndex, size_t slotIndex, Lv2Processor* processor) noexcept;
    void setKeyLv2Enabled(int noteIndex, size_t slotIndex, bool enabled) noexcept;
    void setMasterGain(float gain) noexcept;
    void setLimiterEnabled(bool enabled) noexcept;
    void setSampleNormalizationEnabled(bool enabled) noexcept;
    void setFadeTimeMs(double milliseconds) noexcept;
    void setLoopCrossfade(bool enabled, double milliseconds) noexcept;
    void setGroupGain(int groupIndex, float gain) noexcept;
    void setGroupPan(int groupIndex, float pan) noexcept;
    void setGroupMuted(int groupIndex, bool muted) noexcept;
    void setGroupSolo(int groupIndex, bool solo) noexcept;
    void setMasterLv2Processor(size_t slotIndex, Lv2Processor* processor) noexcept;
    void setMasterLv2Enabled(size_t slotIndex, bool enabled) noexcept;
    [[nodiscard]] float outputPeakLeft() const noexcept;
    [[nodiscard]] float outputPeakRight() const noexcept;
    [[nodiscard]] bool limiterWasActive() const noexcept;
    void noteOn(int noteIndex) noexcept;
    void noteOff(int noteIndex) noexcept;
    void togglePlayback(int noteIndex) noexcept;
    void togglePausePlayback(int noteIndex) noexcept;
    void startRecording(int noteIndex, double maxDurationSeconds, bool autoMode);
    void setRecordingTarget(int noteIndex, double durationSeconds) noexcept;
    void stopRecording(int noteIndex) noexcept;
    void playLayered(const Sample* first, const Sample* second) noexcept;
    void detachSample(const Sample* sample) noexcept;
    void startPreview(const Sample* sample, size_t startFrame, int direction, double speedRatio) noexcept;
    void stopPreview() noexcept;
    void stopAllPlayback() noexcept;

    [[nodiscard]] bool takeFinishedRecording(FinishedRecording* recording);

    void process(
        jack_nframes_t frameCount,
        const jack_default_audio_sample_t* inputLeft,
        const jack_default_audio_sample_t* inputRight,
        jack_default_audio_sample_t* outputLeft,
        jack_default_audio_sample_t* outputRight) noexcept;

private:
    enum class RecordingState : int {
        Idle = 0,
        Recording = 1,
        Finished = 2,
    };

    struct PlaybackVoice {
        bool isPlaying = false;
        bool stopQueued = false;
        double playbackFrame = 0.0;
        size_t fadeInFramesRemaining = 0;
        size_t fadeOutFramesRemaining = 0;
        bool pauseAfterFade = false;
        bool releaseTrackingEnabled = false;
        bool keyHeld = false;
        size_t postReleaseFramesRemaining = 0;
    };

    struct KeySlot {
        std::atomic<const Sample*> sample { nullptr };
        std::atomic<uint32_t> volumePermille { 1000 };
        std::atomic<int32_t> panPermille { 0 };
        std::atomic<int> groupIndex { 0 };
        std::atomic<bool> muted { false };
        std::atomic<bool> solo { false };
        std::atomic<uint32_t> attackTimeUs { 0 };
        std::atomic<uint32_t> releaseTimeUs { 0 };
        std::atomic<size_t> attackFramesCached { 0 };
        std::atomic<size_t> releaseFramesCached { 0 };
        std::atomic<uint32_t> staccatoPermille { 0 };
        std::atomic<size_t> staccatoFramesCached { 0 };
        std::atomic<bool> loopEnabled { true };
        std::atomic<bool> virtualStaccatoEnabled { false };
        std::atomic<bool> selfMixEnabled { false };
        std::atomic<uint32_t> trimStartPermille { 0 };
        std::atomic<uint32_t> trimEndPermille { 1000000 };
        std::atomic<uint32_t> playbackRatePermille { 1000 };
        std::array<std::atomic<Lv2Processor*>, kLv2ChainSize> plugins {};
        std::array<std::atomic<bool>, kLv2ChainSize> pluginEnabled {};
        std::atomic<bool> noteOnRequested { false };
        std::atomic<bool> noteOffRequested { false };
        std::atomic<bool> togglePlaybackRequested { false };
        std::atomic<bool> togglePauseRequested { false };
        std::atomic<int> recordingState { static_cast<int>(RecordingState::Idle) };
        std::atomic<bool> stopRecordingRequested { false };
        std::atomic<size_t> targetFrames { 0 };
        std::vector<float> recordBuffer;
        size_t recordCapacityFrames = 0;
        size_t recordedFrames = 0;
        bool autoMode = false;
        bool autoStarted = false;
        size_t autoSilentFrames = 0;

        PlaybackVoice primaryVoice;
        std::array<PlaybackVoice, 4> overlapVoices {};
    };

    struct LayerSlot {
        std::atomic<const Sample*> sample { nullptr };
        bool isPlaying = false;
        bool stopQueued = false;
        double playbackFrame = 0.0;
        size_t fadeInFramesRemaining = 0;
    };

    struct GroupSlot {
        std::atomic<uint32_t> gainPermille { 1000 };
        std::atomic<int32_t> panPermille { 0 };
        std::atomic<bool> muted { false };
        std::atomic<bool> solo { false };
    };

    struct PreviewSlot {
        std::atomic<const Sample*> sample { nullptr };
        std::atomic<size_t> startFrame { 0 };
        std::atomic<int> direction { 1 };
        std::atomic<uint32_t> speedPermille { 1000 };
        std::atomic<bool> startRequested { false };
        std::atomic<bool> stopRequested { false };
        const Sample* activeSample = nullptr;
        double framePosition = 0.0;
        int activeDirection = 1;
        double activeSpeed = 1.0;
        bool isPlaying = false;
    };

    static constexpr size_t kGroupCount = 4;
    static constexpr size_t kSelfMixVoiceCount = 4;
    static constexpr jack_nframes_t kMaxMixBlockFrames = 8192;
    using MixBuffer = std::array<float, kMaxMixBlockFrames>;

    static void mixSample(
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
        size_t stopAfterFrames = 0) noexcept;

    void processRecordings(
        jack_nframes_t frameCount,
        const jack_default_audio_sample_t* inputLeft,
        const jack_default_audio_sample_t* inputRight) noexcept;
    void processKeyPlayback(
        jack_nframes_t frameCount,
        size_t outputOffset,
        bool anyKeySolo) noexcept;
    void processLayeredPlayback(
        jack_nframes_t frameCount,
        size_t outputOffset) noexcept;
    void processPreviewPlayback(
        jack_nframes_t frameCount,
        size_t outputOffset) noexcept;
    void clearMixBuffers(jack_nframes_t frameCount) noexcept;
    void finalizeMix(
        jack_nframes_t frameCount,
        jack_default_audio_sample_t* outputLeft,
        jack_default_audio_sample_t* outputRight) noexcept;
    [[nodiscard]] bool anyKeySoloActive() const noexcept;
    [[nodiscard]] bool anyGroupSoloActive() const noexcept;
    [[nodiscard]] bool processStopAllRequest() noexcept;
    void refreshTimingCaches() noexcept;
    void refreshKeyTimingCache(KeySlot* slot) noexcept;
    static PlaybackVoice* choosePlaybackVoiceForRetrigger(KeySlot* slot) noexcept;

    std::array<KeySlot, PianoWidget::kKeyCount> keySlots_;
    std::array<LayerSlot, 2> layerSlots_;
    std::array<GroupSlot, kGroupCount> groupSlots_;
    std::array<MixBuffer, kGroupCount> groupLeftBuffers_ {};
    std::array<MixBuffer, kGroupCount> groupRightBuffers_ {};
    MixBuffer workLeftBuffer_ {};
    MixBuffer workRightBuffer_ {};
    MixBuffer masterLeftBuffer_ {};
    MixBuffer masterRightBuffer_ {};
    PreviewSlot previewSlot_;
    std::array<std::atomic<Lv2Processor*>, kLv2ChainSize> masterPlugins_ {};
    std::array<std::atomic<bool>, kLv2ChainSize> masterPluginEnabled_ {};
    std::atomic<bool> layeredToggleRequested_ { false };
    std::atomic<bool> stopAllRequested_ { false };
    std::atomic<uint32_t> sampleRate_ { 48000 };
    std::atomic<uint32_t> masterGainPermille_ { 1000 };
    std::atomic<bool> limiterEnabled_ { true };
    std::atomic<bool> sampleNormalizationEnabled_ { false };
    std::atomic<uint32_t> fadeTimeUs_ { 0 };
    std::atomic<size_t> fadeFramesCached_ { 0 };
    std::atomic<bool> loopCrossfadeEnabled_ { false };
    std::atomic<uint32_t> loopCrossfadeTimeUs_ { 10000 };
    std::atomic<size_t> loopCrossfadeFramesCached_ { 0 };
    std::atomic<uint32_t> outputPeakLeftPermille_ { 0 };
    std::atomic<uint32_t> outputPeakRightPermille_ { 0 };
    std::atomic<bool> limiterActive_ { false };
};
