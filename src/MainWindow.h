#pragma once

#include <array>
#include <unordered_map>

#include <QElapsedTimer>
#include <QMainWindow>
#include <QThread>

#include "AudioEngine.h"
#include "MidiInput.h"
#include "PianoWidget.h"
#include "RootTimingWorker.h"
#include "RubberBandWorker.h"
#include "Sample.h"
#include "SampleLoader.h"

class QAction;
class QCheckBox;
class QCloseEvent;
class QComboBox;
class QDial;
class QDoubleSpinBox;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class SamplePoolWidget;
class QSpinBox;
class QTimer;
class DragLineOverlay;

/**
 * Main application window for SuperLooper.
 */
class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    enum class BeatDetectionScope {
        Beginning = 0,
        End = 1,
        BeginningAndEndAverage = 2,
        EntireSample = 3,
    };

    enum class BeatDetectionMergePolicy {
        PreferHigherConfidence = 0,
        Average = 1,
    };

private slots:
    void connectToJack();
    void connectMidiInput();
    void saveState();
    void loadState();
    void importSfz();
    void exportSfz();
    void showHelp();
    void cycleMode();
    void handleNotePressed(int noteIndex);
    void handleNoteReleased(int noteIndex);
    void handleOctaveChanged(int octaveOffset);
    void handleSampleSelected(Sample* sample);
    void handleSampleRemoved(Sample* sample);
    void handleSampleDroppedOnKey(int noteIndex, Sample* sample);
    void showKeyContextMenu(int noteIndex, const QPoint& globalPos);
    void handleLayeredPlayRequested(Sample* first, Sample* second);
    void showStatusMessage(const QString& message);
    void pollFinishedRecordings();
    void handleRootToggled(bool enabled);
    void handleSyncToggled(bool enabled);
    void handleBarsPerRootChanged(int bars);
    void handleStretchFinished(const StretchResult& result);
    void handleRootTimingFinished(const RootTimingResult& result);
    void handleKeyAutoToggled(bool enabled);
    void handleMidiNoteOn(int midiNote);
    void handleMidiNoteOff(int midiNote);
    void handleEmergencyStop();
    void handleSelectedKeyPlayPause();
    void handleSelectedKeyVolumeChanged(int value);
    void handleSelectedKeyPanChanged(int value);
    void handleSelectedKeyMuteToggled(bool enabled);
    void handleSelectedKeySoloToggled(bool enabled);
    void handleSelectedKeySelfMixToggled(bool enabled);
    void handleSelectedKeyGroupChanged(int index);
    void handleMasterGainChanged(int value);
    void showSettingsDialog();
    void showOptimizationSummary();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    using Lv2SlotUriArray = std::array<QString, AudioEngine::kLv2ChainSize>;
    using Lv2SlotEnabledArray = std::array<bool, AudioEngine::kLv2ChainSize>;
    using Lv2SlotParameterArray = std::array<std::vector<float>, AudioEngine::kLv2ChainSize>;

    enum class AppMode {
        Normal,
        Record,
        Playback,
        Edit,
    };

    enum class RecordingMethod {
        KeyPressesAutomatic = 0,
        ArmedMiddlePedal = 1,
    };

    void createMenus();
    void createCentralLayout();
    void setRecordingMethod(int index);
    void startRecordingForNote(int noteIndex);
    void finishRecordingForNote(int noteIndex);
    void handleMiddlePedalPressed();
    void handleMiddlePedalReleased();
    void selectKeyForEdit(int noteIndex);
    void updateModeLabel();
    void updateWaveformView(Sample* sample);
    void updateSampleFlagControls(Sample* sample);
    void updateKeySettingsControls(int noteIndex);
    void updateSelectedKeyControls(int noteIndex);
    void assignSampleToKey(int noteIndex, Sample* sample);
    void loadSettings();
    void saveSettings() const;
    void applyMixerSettings();
    void updatePeakMeter();
    void updateBottomStatus();
    void setRootSample(Sample* sample);
    void clearRootSample(Sample* sample);
    void queueRootTimingAnalysis(Sample* sample);
    void applyRootTimingTrimToAssignedKeys(Sample* sample, double previousStart, double previousEnd, double analyzedStart, double analyzedEnd);
    void updateSampleTimingStateForSync(Sample* sample);
    [[nodiscard]] double rootTimingDurationSeconds(const Sample* sample) const;
    void updateTempoFromRoot();
    void syncAllSamplesToRoot();
    void queueStretchToRoot(Sample* sample);
    [[nodiscard]] QString sampleTempoStatusText(const Sample* sample) const;
    bool editLv2Parameters(const QString& pluginUri, const QString& title, std::vector<float>* values);
    bool editLv2Chain(
        const QString& title,
        Lv2SlotUriArray* uris,
        Lv2SlotEnabledArray* enabled,
        Lv2SlotParameterArray* parameterValues);
    bool exportAssignedKeymapAsSfzBundle(const QString& exportDirectory, const QString& baseName, QString* sfzPathOut, QString* errorMessage);
    [[nodiscard]] static QString modeName(AppMode mode);
    [[nodiscard]] static QString noteNameForIndex(int noteIndex);
    [[nodiscard]] static double sampleDurationSeconds(const Sample* sample);

    AudioEngine audioEngine_;
    MidiInput midiInput_;
    QAction* connectAction_ = nullptr;
    QAction* connectMidiAction_ = nullptr;
    QComboBox* recordingMethodComboBox_ = nullptr;
    QDoubleSpinBox* loopDurationSpinBox_ = nullptr;
    QSpinBox* barsPerRootSpinBox_ = nullptr;
    QCheckBox* rootCheckBox_ = nullptr;
    QCheckBox* syncCheckBox_ = nullptr;
    QCheckBox* keyAutoCheckBox_ = nullptr;
    QLabel* modeLabel_ = nullptr;
    QLabel* octaveLabel_ = nullptr;
    QLabel* selectedKeyLabel_ = nullptr;
    QLabel* tempoLabel_ = nullptr;
    QLabel* jackSampleRateLabel_ = nullptr;
    QLabel* selectedSampleRateLabel_ = nullptr;
    QDial* selectedKeyVolumeDial_ = nullptr;
    QDial* selectedKeyPanDial_ = nullptr;
    QCheckBox* selectedKeyMuteCheckBox_ = nullptr;
    QCheckBox* selectedKeySoloCheckBox_ = nullptr;
    QCheckBox* selectedKeySelfMixCheckBox_ = nullptr;
    QComboBox* selectedKeyGroupComboBox_ = nullptr;
    QPushButton* selectedKeyPlayPauseButton_ = nullptr;
    QLabel* selectedKeySampleLabel_ = nullptr;
    QDial* masterGainDial_ = nullptr;
    QProgressBar* peakMeter_ = nullptr;
    QLabel* limiterLabel_ = nullptr;
    QPushButton* emergencyStopButton_ = nullptr;
    QLabel* waveformLabel_ = nullptr;
    SamplePoolWidget* samplePoolWidget_ = nullptr;
    PianoWidget* pianoWidget_ = nullptr;
    DragLineOverlay* dragLineOverlay_ = nullptr;
    QTimer* recordingPollTimer_ = nullptr;
    QThread rubberBandThread_;
    RubberBandWorker* rubberBandWorker_ = nullptr;
    QThread rootTimingThread_;
    RootTimingWorker* rootTimingWorker_ = nullptr;
    std::array<Sample*, PianoWidget::kKeyCount> assignedSamples_ {};
    std::array<qint64, PianoWidget::kKeyCount> recordPressTimes_ {};
    std::array<bool, PianoWidget::kKeyCount> perKeyAutoMode_ {};
    std::array<int, PianoWidget::kKeyCount> perKeyVolumePercent_ {};
    std::array<int, PianoWidget::kKeyCount> perKeyPanPercent_ {};
    std::array<bool, PianoWidget::kKeyCount> perKeyMuted_ {};
    std::array<bool, PianoWidget::kKeyCount> perKeySolo_ {};
    std::array<int, PianoWidget::kKeyCount> perKeyGroup_ {};
    std::array<double, PianoWidget::kKeyCount> perKeyAttackMs_ {};
    std::array<double, PianoWidget::kKeyCount> perKeyReleaseMs_ {};
    std::array<int, PianoWidget::kKeyCount> perKeyStaccatoPercent_ {};
    std::array<KeyLoopMode, PianoWidget::kKeyCount> perKeyLoopMode_ {};
    std::array<bool, PianoWidget::kKeyCount> perKeyVirtualStaccatoEnabled_ {};
    std::array<bool, PianoWidget::kKeyCount> perKeySelfMixEnabled_ {};
    std::array<double, PianoWidget::kKeyCount> perKeyTrimStart_ {};
    std::array<double, PianoWidget::kKeyCount> perKeyTrimEnd_ {};
    std::array<double, PianoWidget::kKeyCount> perKeyPlaybackRate_ {};
    std::array<Lv2SlotUriArray, PianoWidget::kKeyCount> perKeyLv2PluginUris_ {};
    std::array<Lv2SlotEnabledArray, PianoWidget::kKeyCount> perKeyLv2Enabled_ {};
    std::array<Lv2SlotParameterArray, PianoWidget::kKeyCount> perKeyLv2ParameterValues_ {};
    std::array<int, 4> groupGainPercent_ {};
    std::array<int, 4> groupPanPercent_ {};
    std::array<bool, 4> groupMuted_ {};
    std::array<bool, 4> groupSolo_ {};
    std::unordered_map<Sample*, int> stretchRequestIds_;
    std::unordered_map<Sample*, int> rootTimingRequestIds_;
    QElapsedTimer appClock_;
    Sample* rootSample_ = nullptr;
    double globalLoopDurationSec_ = 0.0;
    double tempoBpm_ = 0.0;
    int nextStretchRequestId_ = 1;
    int nextRootTimingRequestId_ = 1;
    bool updatingSampleFlagControls_ = false;
    bool updatingKeySettingsControls_ = false;
    int selectedNoteIndex_ = -1;
    int armedRecordingNoteIndex_ = -1;
    int activeMiddlePedalRecordingNoteIndex_ = -1;
    AppMode mode_ = AppMode::Normal;
    RecordingMethod recordingMethod_ = RecordingMethod::KeyPressesAutomatic;
    ResamplerBackend resamplerBackend_ = ResamplerBackend::Automatic;
    ResamplerQuality resamplerQuality_ = ResamplerQuality::Automatic;
    BeatDetectionScope beatDetectionScope_ = BeatDetectionScope::Beginning;
    BeatDetectionMergePolicy beatDetectionMergePolicy_ = BeatDetectionMergePolicy::PreferHigherConfidence;
    double beatDetectionStartLengthSeconds_ = 20.0;
    double beatDetectionEndLengthSeconds_ = 20.0;
    int masterGainPercent_ = 100;
    double fadeTimeMs_ = 5.0;
    bool limiterEnabled_ = true;
    bool sampleNormalizationEnabled_ = false;
    bool loopCrossfadeEnabled_ = false;
    double loopCrossfadeMs_ = 10.0;
    Lv2SlotUriArray masterLv2PluginUris_ {};
    Lv2SlotEnabledArray masterLv2Enabled_ {};
    Lv2SlotParameterArray masterLv2ParameterValues_ {};
};
