#include "AudioEngine.h"

#include <algorithm>

#include <QDebug>

AudioEngine::AudioEngine(QObject* parent)
    : QObject(parent)
{
    for (auto& keyEnabled : keyLv2Enabled_) {
        keyEnabled.fill(true);
    }
    masterLv2Enabled_.fill(true);
}

AudioEngine::~AudioEngine()
{
    disconnectFromJack();
}

bool AudioEngine::connectToJack(QString* errorMessage)
{
    if (client_ != nullptr) {
        return true;
    }

    jack_status_t status {};
    jack_client_t* client = jack_client_open("SuperLooper", JackNullOption, &status);
    if (client == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Unable to open JACK client. Is the JACK server running?");
        }
        return false;
    }

    jack_set_process_callback(client, &AudioEngine::processCallback, this);

    std::array<jack_port_t*, 2> inputPorts {};
    std::array<jack_port_t*, 2> outputPorts {};

    inputPorts[0] = jack_port_register(
        client,
        "input_1",
        JACK_DEFAULT_AUDIO_TYPE,
        JackPortIsInput,
        0);
    inputPorts[1] = jack_port_register(
        client,
        "input_2",
        JACK_DEFAULT_AUDIO_TYPE,
        JackPortIsInput,
        0);
    outputPorts[0] = jack_port_register(
        client,
        "output_1",
        JACK_DEFAULT_AUDIO_TYPE,
        JackPortIsOutput,
        0);
    outputPorts[1] = jack_port_register(
        client,
        "output_2",
        JACK_DEFAULT_AUDIO_TYPE,
        JackPortIsOutput,
        0);

    const bool allPortsRegistered =
        std::all_of(inputPorts.cbegin(), inputPorts.cend(), [](const jack_port_t* port) {
            return port != nullptr;
        })
        && std::all_of(outputPorts.cbegin(), outputPorts.cend(), [](const jack_port_t* port) {
            return port != nullptr;
        });

    if (!allPortsRegistered) {
        jack_client_close(client);
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Unable to register JACK audio ports.");
        }
        return false;
    }

    inputPorts_ = inputPorts;
    outputPorts_ = outputPorts;
    client_ = client;
    loopManager_.setSampleRate(jack_get_sample_rate(client_));
    rebuildLv2Processors();

    if (jack_activate(client_) != 0) {
        disconnectFromJack();
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Unable to activate JACK client.");
        }
        return false;
    }

    autoConnectPhysicalPorts();
    return true;
}

void AudioEngine::disconnectFromJack()
{
    if (client_ == nullptr) {
        return;
    }

    jack_deactivate(client_);
    jack_client_close(client_);

    client_ = nullptr;
    inputPorts_.fill(nullptr);
    outputPorts_.fill(nullptr);
    activeLv2SampleRate_ = 0;
    activeMasterLv2PluginUris_.fill(QString());
    for (size_t noteIndex = 0; noteIndex < keyLv2PluginUris_.size(); ++noteIndex) {
        activeKeyLv2PluginUris_[noteIndex].fill(QString());
        for (size_t slotIndex = 0; slotIndex < kLv2ChainSize; ++slotIndex) {
            loopManager_.setKeyLv2Processor(static_cast<int>(noteIndex), slotIndex, nullptr);
            activeKeyLv2Processors_[noteIndex][slotIndex] = nullptr;
        }
    }
    for (size_t slotIndex = 0; slotIndex < kLv2ChainSize; ++slotIndex) {
        loopManager_.setMasterLv2Processor(slotIndex, nullptr);
        activeMasterLv2Processors_[slotIndex] = nullptr;
    }
    retireAllLv2Processors();
    reapRetiredLv2Processors();
}

bool AudioEngine::isConnected() const noexcept
{
    return client_ != nullptr;
}

void AudioEngine::assignSampleToKey(int noteIndex, const Sample* sample) noexcept
{
    loopManager_.assignSample(noteIndex, sample);
}

void AudioEngine::setKeyVolume(int noteIndex, float volume) noexcept
{
    loopManager_.setKeyVolume(noteIndex, volume);
}

void AudioEngine::setKeyPan(int noteIndex, float pan) noexcept
{
    loopManager_.setKeyPan(noteIndex, pan);
}

void AudioEngine::setKeyMuted(int noteIndex, bool muted) noexcept
{
    loopManager_.setKeyMuted(noteIndex, muted);
}

void AudioEngine::setKeySolo(int noteIndex, bool solo) noexcept
{
    loopManager_.setKeySolo(noteIndex, solo);
}

void AudioEngine::setKeyGroup(int noteIndex, int groupIndex) noexcept
{
    loopManager_.setKeyGroup(noteIndex, groupIndex);
}

void AudioEngine::setKeyAttackMs(int noteIndex, double milliseconds) noexcept
{
    loopManager_.setKeyAttackMs(noteIndex, milliseconds);
}

void AudioEngine::setKeyReleaseMs(int noteIndex, double milliseconds) noexcept
{
    loopManager_.setKeyReleaseMs(noteIndex, milliseconds);
}

void AudioEngine::setKeyStaccato(int noteIndex, float amount) noexcept
{
    loopManager_.setKeyStaccato(noteIndex, amount);
}

void AudioEngine::setKeyLoopMode(int noteIndex, KeyLoopMode mode) noexcept
{
    loopManager_.setKeyLoopMode(noteIndex, mode);
}

void AudioEngine::setKeyVirtualStaccatoEnabled(int noteIndex, bool enabled) noexcept
{
    loopManager_.setKeyVirtualStaccatoEnabled(noteIndex, enabled);
}

void AudioEngine::setKeySelfMixEnabled(int noteIndex, bool enabled) noexcept
{
    loopManager_.setKeySelfMixEnabled(noteIndex, enabled);
}

void AudioEngine::setKeyTrimRange(int noteIndex, double startNormalized, double endNormalized) noexcept
{
    loopManager_.setKeyTrimRange(noteIndex, startNormalized, endNormalized);
}

void AudioEngine::setKeyPlaybackRate(int noteIndex, double playbackRate) noexcept
{
    loopManager_.setKeyPlaybackRate(noteIndex, playbackRate);
}

bool AudioEngine::setKeyLv2PluginUri(int noteIndex, const QString& uri, QString* errorMessage)
{
    return setKeyLv2SlotUri(noteIndex, 0, uri, errorMessage);
}

void AudioEngine::setKeyLv2ParameterValues(int noteIndex, const std::vector<float>& values) noexcept
{
    setKeyLv2SlotParameterValues(noteIndex, 0, values);
}

bool AudioEngine::setKeyLv2SlotUri(int noteIndex, size_t slotIndex, const QString& uri, QString* errorMessage)
{
    if (noteIndex < 0 || noteIndex >= PianoWidget::kKeyCount || slotIndex >= kLv2ChainSize) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Invalid key LV2 slot.");
        }
        return false;
    }

    const QString trimmedUri = uri.trimmed();
    keyLv2PluginUris_[static_cast<size_t>(noteIndex)][slotIndex] = trimmedUri;
    Lv2Processor* oldProcessor = activeKeyLv2Processors_[static_cast<size_t>(noteIndex)][slotIndex];
    if (client_ == nullptr) {
        loopManager_.setKeyLv2Processor(noteIndex, slotIndex, nullptr);
        activeKeyLv2PluginUris_[static_cast<size_t>(noteIndex)][slotIndex].clear();
        activeKeyLv2Processors_[static_cast<size_t>(noteIndex)][slotIndex] = nullptr;
        retireLv2Processor(oldProcessor);
        reapRetiredLv2Processors();
        return true;
    }

    const uint32_t currentSampleRate = audioSampleRate() != 0U ? audioSampleRate() : 48000U;
    if (activeKeyLv2PluginUris_[static_cast<size_t>(noteIndex)][slotIndex] == trimmedUri
        && activeLv2SampleRate_ == currentSampleRate) {
        return true;
    }

    Lv2Processor* processor = nullptr;
    if (!installLv2Processor(trimmedUri, &processor, errorMessage)) {
        return false;
    }

    loopManager_.setKeyLv2Processor(noteIndex, slotIndex, processor);
    activeKeyLv2PluginUris_[static_cast<size_t>(noteIndex)][slotIndex] = trimmedUri;
    activeKeyLv2Processors_[static_cast<size_t>(noteIndex)][slotIndex] = processor;
    retireLv2Processor(oldProcessor);
    reapRetiredLv2Processors();
    return true;
}

void AudioEngine::setKeyLv2SlotEnabled(int noteIndex, size_t slotIndex, bool enabled) noexcept
{
    if (noteIndex < 0 || noteIndex >= PianoWidget::kKeyCount || slotIndex >= kLv2ChainSize) {
        return;
    }

    keyLv2Enabled_[static_cast<size_t>(noteIndex)][slotIndex] = enabled;
    loopManager_.setKeyLv2Enabled(noteIndex, slotIndex, enabled);
}

void AudioEngine::setKeyLv2SlotParameterValues(int noteIndex, size_t slotIndex, const std::vector<float>& values) noexcept
{
    if (noteIndex < 0 || noteIndex >= PianoWidget::kKeyCount || slotIndex >= kLv2ChainSize) {
        return;
    }

    keyLv2ParameterValues_[static_cast<size_t>(noteIndex)][slotIndex] = values;
    applyLv2ParameterValues(activeKeyLv2Processors_[static_cast<size_t>(noteIndex)][slotIndex], values);
}

bool AudioEngine::setMasterLv2PluginUri(const QString& uri, QString* errorMessage)
{
    return setMasterLv2SlotUri(0, uri, errorMessage);
}

void AudioEngine::setMasterLv2ParameterValues(const std::vector<float>& values) noexcept
{
    setMasterLv2SlotParameterValues(0, values);
}

bool AudioEngine::setMasterLv2SlotUri(size_t slotIndex, const QString& uri, QString* errorMessage)
{
    if (slotIndex >= kLv2ChainSize) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Invalid master LV2 slot.");
        }
        return false;
    }

    const QString trimmedUri = uri.trimmed();
    masterLv2PluginUris_[slotIndex] = trimmedUri;
    Lv2Processor* oldProcessor = activeMasterLv2Processors_[slotIndex];
    if (client_ == nullptr) {
        loopManager_.setMasterLv2Processor(slotIndex, nullptr);
        activeMasterLv2PluginUris_[slotIndex].clear();
        activeMasterLv2Processors_[slotIndex] = nullptr;
        retireLv2Processor(oldProcessor);
        reapRetiredLv2Processors();
        return true;
    }

    const uint32_t currentSampleRate = audioSampleRate() != 0U ? audioSampleRate() : 48000U;
    if (activeMasterLv2PluginUris_[slotIndex] == trimmedUri
        && activeLv2SampleRate_ == currentSampleRate) {
        return true;
    }

    Lv2Processor* processor = nullptr;
    if (!installLv2Processor(trimmedUri, &processor, errorMessage)) {
        return false;
    }

    loopManager_.setMasterLv2Processor(slotIndex, processor);
    activeMasterLv2PluginUris_[slotIndex] = trimmedUri;
    activeMasterLv2Processors_[slotIndex] = processor;
    retireLv2Processor(oldProcessor);
    reapRetiredLv2Processors();
    return true;
}

void AudioEngine::setMasterLv2SlotEnabled(size_t slotIndex, bool enabled) noexcept
{
    if (slotIndex >= kLv2ChainSize) {
        return;
    }

    masterLv2Enabled_[slotIndex] = enabled;
    loopManager_.setMasterLv2Enabled(slotIndex, enabled);
}

void AudioEngine::setMasterLv2SlotParameterValues(size_t slotIndex, const std::vector<float>& values) noexcept
{
    if (slotIndex >= kLv2ChainSize) {
        return;
    }

    masterLv2ParameterValues_[slotIndex] = values;
    applyLv2ParameterValues(activeMasterLv2Processors_[slotIndex], values);
}

void AudioEngine::setMasterGain(float gain) noexcept
{
    loopManager_.setMasterGain(gain);
}

void AudioEngine::setLimiterEnabled(bool enabled) noexcept
{
    loopManager_.setLimiterEnabled(enabled);
}

void AudioEngine::setSampleNormalizationEnabled(bool enabled) noexcept
{
    loopManager_.setSampleNormalizationEnabled(enabled);
}

void AudioEngine::setFadeTimeMs(double milliseconds) noexcept
{
    loopManager_.setFadeTimeMs(milliseconds);
}

void AudioEngine::setLoopCrossfade(bool enabled, double milliseconds) noexcept
{
    loopManager_.setLoopCrossfade(enabled, milliseconds);
}

void AudioEngine::setGroupGain(int groupIndex, float gain) noexcept
{
    loopManager_.setGroupGain(groupIndex, gain);
}

void AudioEngine::setGroupPan(int groupIndex, float pan) noexcept
{
    loopManager_.setGroupPan(groupIndex, pan);
}

void AudioEngine::setGroupMuted(int groupIndex, bool muted) noexcept
{
    loopManager_.setGroupMuted(groupIndex, muted);
}

void AudioEngine::setGroupSolo(int groupIndex, bool solo) noexcept
{
    loopManager_.setGroupSolo(groupIndex, solo);
}

float AudioEngine::outputPeakLeft() const noexcept
{
    return loopManager_.outputPeakLeft();
}

float AudioEngine::outputPeakRight() const noexcept
{
    return loopManager_.outputPeakRight();
}

bool AudioEngine::limiterWasActive() const noexcept
{
    return loopManager_.limiterWasActive();
}

void AudioEngine::noteOn(int noteIndex) noexcept
{
    loopManager_.noteOn(noteIndex);
}

void AudioEngine::noteOff(int noteIndex) noexcept
{
    loopManager_.noteOff(noteIndex);
}

void AudioEngine::togglePlayback(int noteIndex) noexcept
{
    loopManager_.togglePlayback(noteIndex);
}

void AudioEngine::togglePausePlayback(int noteIndex) noexcept
{
    loopManager_.togglePausePlayback(noteIndex);
}

void AudioEngine::startRecording(int noteIndex, double maxDurationSeconds, bool autoMode)
{
    loopManager_.startRecording(noteIndex, maxDurationSeconds, autoMode);
}

void AudioEngine::setRecordingTarget(int noteIndex, double durationSeconds) noexcept
{
    loopManager_.setRecordingTarget(noteIndex, durationSeconds);
}

void AudioEngine::stopRecording(int noteIndex) noexcept
{
    loopManager_.stopRecording(noteIndex);
}

void AudioEngine::playLayered(const Sample* first, const Sample* second) noexcept
{
    loopManager_.playLayered(first, second);
}

void AudioEngine::detachSample(const Sample* sample) noexcept
{
    loopManager_.detachSample(sample);
}

void AudioEngine::startPreview(const Sample* sample, size_t startFrame, int direction, double speedRatio) noexcept
{
    loopManager_.startPreview(sample, startFrame, direction, speedRatio);
}

void AudioEngine::stopPreview() noexcept
{
    loopManager_.stopPreview();
}

void AudioEngine::stopAllPlayback() noexcept
{
    loopManager_.stopAllPlayback();
}

bool AudioEngine::takeFinishedRecording(FinishedRecording* recording)
{
    return loopManager_.takeFinishedRecording(recording);
}

uint32_t AudioEngine::audioSampleRate() const noexcept
{
    return loopManager_.sampleRate();
}

int AudioEngine::processCallback(jack_nframes_t frameCount, void* userData) noexcept
{
    auto* engine = static_cast<AudioEngine*>(userData);
    if (engine == nullptr) {
        return 0;
    }

    return engine->process(frameCount);
}

int AudioEngine::process(jack_nframes_t frameCount) noexcept
{
    activeCallbacks_.fetch_add(1, std::memory_order_acq_rel);
    const uint64_t generation = callbackGeneration_.fetch_add(1, std::memory_order_acq_rel) + 1U;
    auto* inputLeft = static_cast<jack_default_audio_sample_t*>(
        jack_port_get_buffer(inputPorts_[0], frameCount));
    auto* inputRight = static_cast<jack_default_audio_sample_t*>(
        jack_port_get_buffer(inputPorts_[1], frameCount));
    auto* outputLeft = static_cast<jack_default_audio_sample_t*>(
        jack_port_get_buffer(outputPorts_[0], frameCount));
    auto* outputRight = static_cast<jack_default_audio_sample_t*>(
        jack_port_get_buffer(outputPorts_[1], frameCount));

    loopManager_.process(frameCount, inputLeft, inputRight, outputLeft, outputRight);

    completedCallbackGeneration_.store(generation, std::memory_order_release);
    activeCallbacks_.fetch_sub(1, std::memory_order_acq_rel);

    return 0;
}

void AudioEngine::autoConnectPhysicalPorts() noexcept
{
    if (client_ == nullptr) {
        return;
    }

    const char** capturePorts = jack_get_ports(
        client_,
        nullptr,
        JACK_DEFAULT_AUDIO_TYPE,
        JackPortIsPhysical | JackPortIsOutput);
    if (capturePorts != nullptr) {
        for (size_t index = 0; index < inputPorts_.size() && capturePorts[index] != nullptr; ++index) {
            jack_connect(client_, capturePorts[index], jack_port_name(inputPorts_[index]));
        }
        jack_free(capturePorts);
    }

    const char** playbackPorts = jack_get_ports(
        client_,
        nullptr,
        JACK_DEFAULT_AUDIO_TYPE,
        JackPortIsPhysical | JackPortIsInput);
    if (playbackPorts != nullptr) {
        for (size_t index = 0; index < outputPorts_.size() && playbackPorts[index] != nullptr; ++index) {
            jack_connect(client_, jack_port_name(outputPorts_[index]), playbackPorts[index]);
        }
        jack_free(playbackPorts);
    }
}

void AudioEngine::rebuildLv2Processors()
{
    activeLv2SampleRate_ = audioSampleRate() != 0U ? audioSampleRate() : 48000U;
    for (int noteIndex = 0; noteIndex < PianoWidget::kKeyCount; ++noteIndex) {
        for (size_t slotIndex = 0; slotIndex < kLv2ChainSize; ++slotIndex) {
            QString ignoredError;
            Lv2Processor* processor = nullptr;
            installLv2Processor(keyLv2PluginUris_[static_cast<size_t>(noteIndex)][slotIndex], &processor, &ignoredError);
            if (!ignoredError.isEmpty()) {
                qWarning().noquote() << ignoredError;
            }
            retireLv2Processor(activeKeyLv2Processors_[static_cast<size_t>(noteIndex)][slotIndex]);
            loopManager_.setKeyLv2Processor(noteIndex, slotIndex, processor);
            loopManager_.setKeyLv2Enabled(noteIndex, slotIndex, keyLv2Enabled_[static_cast<size_t>(noteIndex)][slotIndex]);
            activeKeyLv2Processors_[static_cast<size_t>(noteIndex)][slotIndex] = processor;
            applyLv2ParameterValues(processor, keyLv2ParameterValues_[static_cast<size_t>(noteIndex)][slotIndex]);
            activeKeyLv2PluginUris_[static_cast<size_t>(noteIndex)][slotIndex] =
                processor != nullptr ? keyLv2PluginUris_[static_cast<size_t>(noteIndex)][slotIndex] : QString();
        }
    }

    for (size_t slotIndex = 0; slotIndex < kLv2ChainSize; ++slotIndex) {
        QString ignoredError;
        Lv2Processor* masterProcessor = nullptr;
        installLv2Processor(masterLv2PluginUris_[slotIndex], &masterProcessor, &ignoredError);
        if (!ignoredError.isEmpty()) {
            qWarning().noquote() << ignoredError;
        }
        retireLv2Processor(activeMasterLv2Processors_[slotIndex]);
        loopManager_.setMasterLv2Processor(slotIndex, masterProcessor);
        loopManager_.setMasterLv2Enabled(slotIndex, masterLv2Enabled_[slotIndex]);
        activeMasterLv2Processors_[slotIndex] = masterProcessor;
        applyLv2ParameterValues(masterProcessor, masterLv2ParameterValues_[slotIndex]);
        activeMasterLv2PluginUris_[slotIndex] =
            masterProcessor != nullptr ? masterLv2PluginUris_[slotIndex] : QString();
    }
    reapRetiredLv2Processors();
}

bool AudioEngine::installLv2Processor(const QString& uri, Lv2Processor** destination, QString* errorMessage)
{
    if (destination == nullptr) {
        return false;
    }

    *destination = nullptr;
    if (uri.trimmed().isEmpty()) {
        return true;
    }

    std::unique_ptr<Lv2Processor> processor = Lv2Host::createProcessor(
        uri,
        audioSampleRate() != 0U ? static_cast<double>(audioSampleRate()) : 48000.0,
        errorMessage);
    if (!processor || !processor->isValid()) {
        return false;
    }

    *destination = processor.get();
    activeLv2Processors_.push_back(std::move(processor));
    return true;
}

void AudioEngine::applyLv2ParameterValues(Lv2Processor* processor, const std::vector<float>& values) noexcept
{
    if (processor == nullptr) {
        return;
    }

    const size_t count = std::min(values.size(), processor->controlPortCount());
    for (size_t index = 0; index < count; ++index) {
        processor->setControlValue(index, values[index]);
    }
}

void AudioEngine::retireLv2Processor(Lv2Processor* processor)
{
    if (processor == nullptr) {
        return;
    }

    auto it = std::find_if(
        activeLv2Processors_.begin(),
        activeLv2Processors_.end(),
        [processor](const std::unique_ptr<Lv2Processor>& candidate) {
            return candidate.get() == processor;
        });
    if (it == activeLv2Processors_.end()) {
        return;
    }

    RetiredLv2Processor retired;
    retired.processor = std::move(*it);
    retired.retireAfterGeneration = callbackGeneration_.load(std::memory_order_acquire) + 2U;
    activeLv2Processors_.erase(it);

    const std::lock_guard<std::mutex> lock(retiredLv2Mutex_);
    retiredLv2Processors_.push_back(std::move(retired));
}

void AudioEngine::retireAllLv2Processors()
{
    for (std::unique_ptr<Lv2Processor>& processor : activeLv2Processors_) {
        RetiredLv2Processor retired;
        retired.processor = std::move(processor);
        retired.retireAfterGeneration = completedCallbackGeneration_.load(std::memory_order_acquire);
        if (retired.processor) {
            const std::lock_guard<std::mutex> lock(retiredLv2Mutex_);
            retiredLv2Processors_.push_back(std::move(retired));
        }
    }
    activeLv2Processors_.clear();
}

void AudioEngine::reapRetiredLv2Processors() noexcept
{
    if (activeCallbacks_.load(std::memory_order_acquire) != 0U) {
        return;
    }

    const uint64_t completedGeneration = completedCallbackGeneration_.load(std::memory_order_acquire);
    const std::lock_guard<std::mutex> lock(retiredLv2Mutex_);
    retiredLv2Processors_.erase(
        std::remove_if(
            retiredLv2Processors_.begin(),
            retiredLv2Processors_.end(),
            [completedGeneration](const RetiredLv2Processor& retired) {
                return retired.retireAfterGeneration <= completedGeneration;
            }),
        retiredLv2Processors_.end());
}
