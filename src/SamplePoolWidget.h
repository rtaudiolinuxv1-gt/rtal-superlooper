#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include <QElapsedTimer>
#include <QImage>
#include <QScrollArea>
#include <QThread>

#include "Sample.h"
#include "SampleLoader.h"

class QGridLayout;
class QTimer;
class QUndoStack;
class SampleBlockWidget;
class TrimRangeCommand;

/**
 * Displays imported samples as draggable blocks and owns sample data.
 */
class SamplePoolWidget final : public QScrollArea
{
    Q_OBJECT

public:
    explicit SamplePoolWidget(QWidget* parent = nullptr);
    ~SamplePoolWidget() override;

    [[nodiscard]] Sample* selectedSample() const noexcept;
    [[nodiscard]] std::vector<Sample*> allSamples() const;
    Sample* addRecordedSample(const QString& name, std::vector<float> data, uint32_t sampleRate, size_t frames);
    Sample* addStateSample(const LoadedSample& loadedSample);
    void clearSamples();
    void replaceSampleAudio(Sample* sample, std::vector<float> data, uint32_t sampleRate, size_t frames);
    void refreshSample(Sample* sample);
    [[nodiscard]] bool saveSampleToWav(Sample* sample, const QString& filePath, QString* errorMessage);
    void setTargetSampleRate(uint32_t sampleRate);
    void setResamplerBackend(ResamplerBackend backend);
    void setResamplerQuality(ResamplerQuality quality);
    void setApplyNormalization(bool enabled) noexcept;
    void resampleAllToTargetRate();

signals:
    void loadFileRequested(const QString& filePath);
    void sampleSelected(Sample* sample);
    void statusMessage(const QString& message);
    void layeredPlayRequested(Sample* first, Sample* second);
    void appendedSampleCreated(Sample* sample);
    void sampleRemoved(Sample* sample);
    void markerPreviewStarted(Sample* sample, size_t startFrame, int direction, double speedRatio);
    void markerPreviewStopped();

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void addLoadedSample(const LoadedSample& loadedSample);
    void selectSample(Sample* sample);
    void handleThumbnailReady(quint64 sampleToken, quint64 requestId, const QImage& image);

private:
    friend class TrimRangeCommand;

    enum class MarkerEditTarget {
        None,
        Start,
        End,
    };

    void addSample(std::unique_ptr<Sample> sample);
    void rebuildGrid();
    void relabelSelected(char label);
    void requestLayeredPlay();
    void appendLabelledSamples();
    void enterMarkerEditMode(MarkerEditTarget target);
    void startMarkerPreview(int direction, Qt::KeyboardModifiers modifiers);
    void updateMarkerPreviewPosition();
    void stopMarkerPreview(bool commit);
    void finishMarkerEditMode();
    void autoTrimSelected();
    void cloneSelectedTrimmedRegion();
    void labelSample(Sample* sample);
    void exportSample(Sample* sample);
    void exportInMemorySamplesToDirectory();
    void navigateSelection(int delta);
    void deleteSelectedSample();
    void removeSample(Sample* sample);
    void refreshThumbnails();
    void requestThumbnailRefresh(Sample* sample);
    void applyTrimRange(Sample* sample, double startFrame, double endFrame);

    [[nodiscard]] Sample* labelledSample(char label) const;
    [[nodiscard]] std::unique_ptr<Sample> makeAppendedSample(const Sample& first, const Sample& second);
    [[nodiscard]] std::unique_ptr<Sample> makeTrimmedClone(const Sample& source) const;
    [[nodiscard]] std::unique_ptr<Sample> makeTrimmedClone(const Sample& source, double startFrame, double endFrame) const;
    [[nodiscard]] static bool writeSampleToWav(const Sample& sample, const QString& filePath, QString* errorMessage);
    [[nodiscard]] static QString safeFileBaseName(const QString& name);
    [[nodiscard]] static double previewSpeedForModifiers(Qt::KeyboardModifiers modifiers);

    QWidget* contentWidget_ = nullptr;
    QGridLayout* gridLayout_ = nullptr;
    std::vector<std::unique_ptr<Sample>> samples_;
    std::vector<SampleBlockWidget*> blocks_;
    Sample* selectedSample_ = nullptr;
    int gridColumnCount_ = 1;
    QUndoStack* undoStack_ = nullptr;
    MarkerEditTarget markerEditTarget_ = MarkerEditTarget::None;
    QTimer* markerPreviewTimer_ = nullptr;
    QElapsedTimer markerPreviewClock_;
    size_t markerPreviewOriginFrame_ = 0;
    double markerPreviewOriginalStart_ = 0.0;
    double markerPreviewOriginalEnd_ = 1.0;
    int markerPreviewDirection_ = 1;
    double markerPreviewSpeedRatio_ = 1.0;
    bool markerPreviewActive_ = false;
    QThread loaderThread_;
    SampleLoader* loader_ = nullptr;
    uint32_t targetSampleRate_ = 0;
    ResamplerBackend resamplerBackend_ = ResamplerBackend::Automatic;
    ResamplerQuality resamplerQuality_ = ResamplerQuality::Automatic;
    bool applyNormalization_ = false;
    std::unordered_map<Sample*, quint64> thumbnailRequestIds_;
    quint64 nextThumbnailRequestId_ = 1;
};
