#pragma once

#include <atomic>
#include <array>
#include <memory>
#include <mutex>
#include <vector>

#include <jack/jack.h>
#include <QObject>
#include <QString>

#include "Lv2Host.h"
#include "LoopManager.h"

/**
 * Owns the JACK client and real-time process callback for SuperLooper.
 *
 * The JACK callback must remain real-time safe: no allocation, no locks, no
 * Qt signal emission, and no logging from the audio thread.
 */
class AudioEngine final : public QObject
{
    Q_OBJECT

public:
    static constexpr size_t kLv2ChainSize = LoopManager::kLv2ChainSize;

    explicit AudioEngine(QObject* parent = nullptr);
    ~AudioEngine() override;

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    /**
     * Opens, configures, and activates the JACK client.
     *
     * @param errorMessage Optional destination for a user-facing error.
     * @return true when JACK is connected and activated.
     */
    bool connectToJack(QString* errorMessage = nullptr);

    /**
     * Deactivates and closes the JACK client if it is open.
     */
    void disconnectFromJack();

    /**
     * @return true when a JACK client is currently open.
     */
    [[nodiscard]] bool isConnected() const noexcept;

    void assignSampleToKey(int noteIndex, const Sample* sample) noexcept;
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
    bool setKeyLv2PluginUri(int noteIndex, const QString& uri, QString* errorMessage = nullptr);
    void setKeyLv2ParameterValues(int noteIndex, const std::vector<float>& values) noexcept;
    bool setKeyLv2SlotUri(int noteIndex, size_t slotIndex, const QString& uri, QString* errorMessage = nullptr);
    void setKeyLv2SlotEnabled(int noteIndex, size_t slotIndex, bool enabled) noexcept;
    void setKeyLv2SlotParameterValues(int noteIndex, size_t slotIndex, const std::vector<float>& values) noexcept;
    void setMasterGain(float gain) noexcept;
    void setLimiterEnabled(bool enabled) noexcept;
    void setSampleNormalizationEnabled(bool enabled) noexcept;
    void setFadeTimeMs(double milliseconds) noexcept;
    void setLoopCrossfade(bool enabled, double milliseconds) noexcept;
    void setGroupGain(int groupIndex, float gain) noexcept;
    void setGroupPan(int groupIndex, float pan) noexcept;
    void setGroupMuted(int groupIndex, bool muted) noexcept;
    void setGroupSolo(int groupIndex, bool solo) noexcept;
    bool setMasterLv2PluginUri(const QString& uri, QString* errorMessage = nullptr);
    void setMasterLv2ParameterValues(const std::vector<float>& values) noexcept;
    bool setMasterLv2SlotUri(size_t slotIndex, const QString& uri, QString* errorMessage = nullptr);
    void setMasterLv2SlotEnabled(size_t slotIndex, bool enabled) noexcept;
    void setMasterLv2SlotParameterValues(size_t slotIndex, const std::vector<float>& values) noexcept;
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
    [[nodiscard]] uint32_t audioSampleRate() const noexcept;

private:
    struct RetiredLv2Processor {
        std::unique_ptr<Lv2Processor> processor;
        uint64_t retireAfterGeneration = 0;
    };

    static int processCallback(jack_nframes_t frameCount, void* userData) noexcept;
    int process(jack_nframes_t frameCount) noexcept;
    void autoConnectPhysicalPorts() noexcept;
    void rebuildLv2Processors();
    bool installLv2Processor(const QString& uri, Lv2Processor** destination, QString* errorMessage);
    void applyLv2ParameterValues(Lv2Processor* processor, const std::vector<float>& values) noexcept;
    void retireLv2Processor(Lv2Processor* processor);
    void retireAllLv2Processors();
    void reapRetiredLv2Processors() noexcept;

    jack_client_t* client_ = nullptr;
    std::array<jack_port_t*, 2> inputPorts_ {};
    std::array<jack_port_t*, 2> outputPorts_ {};
    LoopManager loopManager_;
    std::array<std::array<QString, kLv2ChainSize>, PianoWidget::kKeyCount> keyLv2PluginUris_ {};
    std::array<std::array<QString, kLv2ChainSize>, PianoWidget::kKeyCount> activeKeyLv2PluginUris_ {};
    std::array<std::array<bool, kLv2ChainSize>, PianoWidget::kKeyCount> keyLv2Enabled_ {};
    std::array<std::array<std::vector<float>, kLv2ChainSize>, PianoWidget::kKeyCount> keyLv2ParameterValues_ {};
    std::array<std::array<Lv2Processor*, kLv2ChainSize>, PianoWidget::kKeyCount> activeKeyLv2Processors_ {};
    std::array<QString, kLv2ChainSize> masterLv2PluginUris_ {};
    std::array<QString, kLv2ChainSize> activeMasterLv2PluginUris_ {};
    std::array<bool, kLv2ChainSize> masterLv2Enabled_ {};
    std::array<std::vector<float>, kLv2ChainSize> masterLv2ParameterValues_ {};
    std::array<Lv2Processor*, kLv2ChainSize> activeMasterLv2Processors_ {};
    uint32_t activeLv2SampleRate_ = 0;
    std::atomic<uint64_t> callbackGeneration_ { 0 };
    std::atomic<uint64_t> completedCallbackGeneration_ { 0 };
    std::atomic<uint32_t> activeCallbacks_ { 0 };
    std::vector<std::unique_ptr<Lv2Processor>> activeLv2Processors_;
    std::mutex retiredLv2Mutex_;
    std::vector<RetiredLv2Processor> retiredLv2Processors_;
};
