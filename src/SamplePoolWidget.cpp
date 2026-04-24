#include "SamplePoolWidget.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

#include <QAbstractButton>
#include <QApplication>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDir>
#include <QDrag>
#include <QDragEnterEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMetaObject>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QImage>
#include <QPointer>
#include <QPushButton>
#include <QRunnable>
#include <QThreadPool>
#include <QTimer>
#include <QUndoCommand>
#include <QUndoStack>
#include <QUrl>
#include <QVBoxLayout>

#include <sndfile.h>

namespace {
constexpr const char* kSampleMimeType = "application/x-superlooper-sample";
constexpr const char* kSampleDragSourceMimeType = "application/x-superlooper-sample-source";
constexpr int kBlockSize = 84;
constexpr int kBlockSpacing = 10;
constexpr int kThumbnailWidth = 640;
constexpr int kThumbnailHeight = 96;
constexpr float kTrimThreshold = 0.005F;
constexpr double kDialogDefaultNudgeSeconds = 0.010;
constexpr double kDialogFineNudgeSeconds = 0.001;

QByteArray samplePointerToMime(const Sample* sample)
{
    return QByteArray::number(reinterpret_cast<quintptr>(sample));
}

QByteArray pointToMime(const QPoint& point)
{
    return QByteArray::number(point.x()) + "," + QByteArray::number(point.y());
}

std::pair<size_t, size_t> trimmedFrameRange(const Sample& sample)
{
    const size_t start = static_cast<size_t>(
        std::clamp(sample.startFrame, 0.0, 1.0) * static_cast<double>(sample.frames));
    const size_t end = static_cast<size_t>(
        std::clamp(sample.endFrame, 0.0, 1.0) * static_cast<double>(sample.frames));
    return { std::min(start, sample.frames), std::min(std::max(end, start), sample.frames) };
}

size_t frameFromNormalized(const Sample& sample, double normalized)
{
    return std::min(
        sample.frames,
        static_cast<size_t>(std::round(std::clamp(normalized, 0.0, 1.0) * static_cast<double>(sample.frames))));
}

double normalizedFromFrame(const Sample& sample, size_t frame)
{
    if (sample.frames == 0U) {
        return 0.0;
    }

    return static_cast<double>(std::min(frame, sample.frames)) / static_cast<double>(sample.frames);
}

QString formatSampleTime(const Sample& sample, double normalized)
{
    if (sample.sampleRate == 0U) {
        return QStringLiteral("00.00.000");
    }

    const size_t frame = frameFromNormalized(sample, normalized);
    const auto totalMs = static_cast<qint64>(
        std::llround((static_cast<double>(frame) * 1000.0) / static_cast<double>(sample.sampleRate)));
    const qint64 minutes = totalMs / 60000;
    const qint64 seconds = (totalMs / 1000) % 60;
    const qint64 milliseconds = totalMs % 1000;
    return QStringLiteral("%1.%2.%3")
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'))
        .arg(milliseconds, 3, 10, QLatin1Char('0'));
}

float framePeak(const Sample& sample, size_t frame)
{
    const size_t offset = frame * 2U;
    if (offset + 1U >= sample.data.size()) {
        return 0.0F;
    }

    return std::max(std::abs(sample.data[offset]), std::abs(sample.data[offset + 1U]));
}

struct ThumbnailSnapshot {
    std::vector<float> data;
    size_t frames = 0;
    double startFrame = 0.0;
    double endFrame = 1.0;
    QColor color;
};

QImage renderThumbnailImage(const ThumbnailSnapshot& sample)
{
    QImage image(kThumbnailWidth, kThumbnailHeight, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(28, 31, 34));

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.fillRect(image.rect(), QColor(28, 31, 34));

    if (sample.frames == 0U || sample.data.empty()) {
        return image;
    }

    const int centerY = kThumbnailHeight / 2;
    painter.setPen(QPen(sample.color.lighter(130), 1));

    for (int x = 0; x < kThumbnailWidth; ++x) {
        const size_t startFrame = static_cast<size_t>(
            (static_cast<double>(x) / static_cast<double>(kThumbnailWidth)) * static_cast<double>(sample.frames));
        const size_t endFrame = static_cast<size_t>(
            (static_cast<double>(x + 1) / static_cast<double>(kThumbnailWidth)) * static_cast<double>(sample.frames));

        float minValue = 0.0F;
        float maxValue = 0.0F;
        for (size_t frame = startFrame; frame < std::max(endFrame, startFrame + 1U) && frame < sample.frames; ++frame) {
            const size_t offset = frame * 2U;
            const float mono = (sample.data[offset] + sample.data[offset + 1U]) * 0.5F;
            minValue = std::min(minValue, mono);
            maxValue = std::max(maxValue, mono);
        }

        const int y1 = centerY - static_cast<int>(std::round(maxValue * static_cast<float>(centerY - 4)));
        const int y2 = centerY - static_cast<int>(std::round(minValue * static_cast<float>(centerY - 4)));
        painter.drawLine(x, y1, x, y2);
    }

    const int startX = static_cast<int>(std::round(sample.startFrame * static_cast<double>(kThumbnailWidth)));
    const int endX = static_cast<int>(std::round(sample.endFrame * static_cast<double>(kThumbnailWidth)));
    painter.setPen(QPen(QColor(233, 90, 76), 2));
    painter.drawLine(startX, 0, startX, kThumbnailHeight);
    painter.drawLine(endX, 0, endX, kThumbnailHeight);

    return image;
}

class ThumbnailTask final : public QRunnable
{
public:
    ThumbnailTask(SamplePoolWidget* owner, quint64 sampleToken, quint64 requestId, ThumbnailSnapshot snapshot)
        : owner_(owner)
        , sampleToken_(sampleToken)
        , requestId_(requestId)
        , snapshot_(std::move(snapshot))
    {
    }

    void run() override
    {
        const QImage image = renderThumbnailImage(snapshot_);
        if (owner_.isNull()) {
            return;
        }

        QMetaObject::invokeMethod(
            owner_.data(),
            "handleThumbnailReady",
            Qt::QueuedConnection,
            Q_ARG(quint64, sampleToken_),
            Q_ARG(quint64, requestId_),
            Q_ARG(QImage, image));
    }

private:
    QPointer<SamplePoolWidget> owner_;
    quint64 sampleToken_ = 0;
    quint64 requestId_ = 0;
    ThumbnailSnapshot snapshot_;
};

ThumbnailSnapshot makeThumbnailSnapshot(const Sample& sample)
{
    ThumbnailSnapshot snapshot;
    snapshot.data = sample.data;
    snapshot.frames = sample.frames;
    snapshot.startFrame = sample.startFrame;
    snapshot.endFrame = sample.endFrame;
    snapshot.color = sample.color;
    return snapshot;
}
}

class SampleBlockWidget final : public QWidget
{
    Q_OBJECT

public:
    explicit SampleBlockWidget(Sample* sample, QWidget* parent = nullptr)
        : QWidget(parent)
        , sample_(sample)
    {
        setFixedSize(kBlockSize, kBlockSize);
        setCursor(Qt::OpenHandCursor);
    }

    [[nodiscard]] Sample* sample() const noexcept
    {
        return sample_;
    }

    void setSelected(bool selected)
    {
        if (selected_ == selected) {
            return;
        }

        selected_ = selected;
        update();
    }

signals:
    void selected(Sample* sample);
    void labelRequested(Sample* sample);
    void exportRequested(Sample* sample);
    void gainRequested(Sample* sample);

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton) {
            QWidget::mousePressEvent(event);
            return;
        }

        dragStart_ = event->pos();
        emit selected(sample_);
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if ((event->buttons() & Qt::LeftButton) == 0) {
            QWidget::mouseMoveEvent(event);
            return;
        }

        if ((event->pos() - dragStart_).manhattanLength() < QApplication::startDragDistance()) {
            return;
        }

        auto* drag = new QDrag(this);
        auto* mimeData = new QMimeData;
        mimeData->setData(kSampleMimeType, samplePointerToMime(sample_));
        mimeData->setData(kSampleDragSourceMimeType, pointToMime(mapToGlobal(rect().center())));
        mimeData->setText(sample_->name);
        drag->setMimeData(mimeData);
        QPixmap transparentPixmap(1, 1);
        transparentPixmap.fill(Qt::transparent);
        drag->setPixmap(transparentPixmap);
        drag->setHotSpot(QPoint(0, 0));
        drag->exec(Qt::CopyAction);
    }

    void contextMenuEvent(QContextMenuEvent* event) override
    {
        emit selected(sample_);
        QMenu menu(this);
        QAction* labelAction = menu.addAction(QStringLiteral("Label Sample..."));
        QAction* gainAction = menu.addAction(QStringLiteral("Sample Gain..."));
        QAction* exportAction = menu.addAction(QStringLiteral("Export Sample"));
        QAction* selectedAction = menu.exec(event->globalPos());
        if (selectedAction == labelAction) {
            emit labelRequested(sample_);
        } else if (selectedAction == gainAction) {
            emit gainRequested(sample_);
        } else if (selectedAction == exportAction) {
            emit exportRequested(sample_);
        }
    }

    void paintEvent(QPaintEvent* event) override
    {
        Q_UNUSED(event)

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const QRect blockRect = rect().adjusted(3, 3, -3, -3);
        painter.setPen(QPen(selected_ ? QColor(255, 255, 255) : QColor(58, 64, 70), selected_ ? 3 : 1));
        painter.setBrush(sample_->color);
        painter.drawRoundedRect(blockRect, 6.0, 6.0);

        if (!sample_->thumbnail.isNull()) {
            painter.drawPixmap(blockRect.adjusted(7, 14, -7, -28), sample_->thumbnail);
        }

        painter.setPen(Qt::white);
        QFont nameFont = font();
        nameFont.setPointSize(8);
        painter.setFont(nameFont);
        const QString blockText = sample_->userLabel.isEmpty() ? sample_->name : sample_->userLabel;
        painter.drawText(blockRect.adjusted(5, 46, -5, -5), Qt::AlignHCenter | Qt::AlignBottom, blockText);

        if (sample_->label != '\0') {
            const QRect labelRect(blockRect.right() - 22, blockRect.top() + 5, 16, 16);
            painter.setBrush(QColor(20, 24, 28));
            painter.setPen(Qt::NoPen);
            painter.drawEllipse(labelRect);
            painter.setPen(Qt::white);
            painter.drawText(labelRect, Qt::AlignCenter, QString(QChar::fromLatin1(sample_->label)));
        }
    }

private:
    Sample* sample_ = nullptr;
    QPoint dragStart_;
    bool selected_ = false;
};

class TrimRangeCommand final : public QUndoCommand
{
public:
    TrimRangeCommand(
        SamplePoolWidget* pool,
        Sample* sample,
        double oldStart,
        double oldEnd,
        double newStart,
        double newEnd,
        const QString& text)
        : QUndoCommand(text)
        , pool_(pool)
        , sample_(sample)
        , oldStart_(oldStart)
        , oldEnd_(oldEnd)
        , newStart_(newStart)
        , newEnd_(newEnd)
    {
    }

    void undo() override
    {
        pool_->applyTrimRange(sample_, oldStart_, oldEnd_);
    }

    void redo() override
    {
        pool_->applyTrimRange(sample_, newStart_, newEnd_);
    }

private:
    SamplePoolWidget* pool_ = nullptr;
    Sample* sample_ = nullptr;
    double oldStart_ = 0.0;
    double oldEnd_ = 1.0;
    double newStart_ = 0.0;
    double newEnd_ = 1.0;
};

class TrimRangeDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit TrimRangeDialog(const Sample& sample, QWidget* parent = nullptr)
        : QDialog(parent)
        , sample_(sample)
        , startFrame_(frameFromNormalized(sample, sample.startFrame))
        , endFrame_(frameFromNormalized(sample, sample.endFrame))
    {
        setWindowTitle(QStringLiteral("Create Trimmed Sample"));
        auto* layout = new QVBoxLayout(this);
        valueLabel_ = new QLabel(this);
        valueLabel_->setAlignment(Qt::AlignCenter);
        valueLabel_->setMinimumWidth(420);
        layout->addWidget(valueLabel_);
        updateLabel();
    }

    [[nodiscard]] double startFrameNormalized() const
    {
        return normalizedFromFrame(sample_, startFrame_);
    }

    [[nodiscard]] double endFrameNormalized() const
    {
        return normalizedFromFrame(sample_, endFrame_);
    }

protected:
    void keyPressEvent(QKeyEvent* event) override
    {
        if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right) {
            editingStart_ = !editingStart_;
            updateLabel();
            event->accept();
            return;
        }

        if (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down) {
            const int direction = event->key() == Qt::Key_Up ? 1 : -1;
            nudgeSelected(direction, event->modifiers());
            event->accept();
            return;
        }

        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            accept();
            return;
        }

        QDialog::keyPressEvent(event);
    }

private:
    [[nodiscard]] size_t nudgeFrames(Qt::KeyboardModifiers modifiers) const
    {
        if (modifiers & Qt::AltModifier) {
            return 1U;
        }

        const double seconds = (modifiers & Qt::ShiftModifier) != 0 ? kDialogFineNudgeSeconds : kDialogDefaultNudgeSeconds;
        return std::max<size_t>(1U, static_cast<size_t>(std::llround(seconds * static_cast<double>(sample_.sampleRate))));
    }

    void nudgeSelected(int direction, Qt::KeyboardModifiers modifiers)
    {
        const size_t amount = nudgeFrames(modifiers);
        if (editingStart_) {
            if (direction > 0) {
                startFrame_ = std::min(startFrame_ + amount, endFrame_ > 0U ? endFrame_ - 1U : 0U);
            } else {
                startFrame_ = amount > startFrame_ ? 0U : startFrame_ - amount;
            }
        } else {
            if (direction > 0) {
                endFrame_ = std::min(endFrame_ + amount, sample_.frames);
            } else {
                endFrame_ = std::max(startFrame_ + 1U, amount > endFrame_ ? 0U : endFrame_ - amount);
            }
        }

        updateLabel();
    }

    void updateLabel()
    {
        const QString startMarker = editingStart_ ? QStringLiteral(">") : QStringLiteral(" ");
        const QString endMarker = editingStart_ ? QStringLiteral(" ") : QStringLiteral(">");
        valueLabel_->setText(QStringLiteral("%1starting point=%2  %3ending=%4")
            .arg(startMarker, formatSampleTime(sample_, startFrameNormalized()), endMarker, formatSampleTime(sample_, endFrameNormalized())));
    }

    const Sample& sample_;
    QLabel* valueLabel_ = nullptr;
    size_t startFrame_ = 0;
    size_t endFrame_ = 0;
    bool editingStart_ = true;
};

SamplePoolWidget::SamplePoolWidget(QWidget* parent)
    : QScrollArea(parent)
{
    qRegisterMetaType<LoadedSample>("LoadedSample");

    setAcceptDrops(true);
    setFocusPolicy(Qt::StrongFocus);
    setFrameShape(QFrame::StyledPanel);
    setWidgetResizable(true);
    undoStack_ = new QUndoStack(this);
    markerPreviewTimer_ = new QTimer(this);
    markerPreviewTimer_->setInterval(30);
    connect(markerPreviewTimer_, &QTimer::timeout, this, &SamplePoolWidget::updateMarkerPreviewPosition);

    contentWidget_ = new QWidget(this);
    gridLayout_ = new QGridLayout(contentWidget_);
    gridLayout_->setContentsMargins(kBlockSpacing, kBlockSpacing, kBlockSpacing, kBlockSpacing);
    gridLayout_->setSpacing(kBlockSpacing);
    gridLayout_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    setWidget(contentWidget_);

    loader_ = new SampleLoader;
    loader_->moveToThread(&loaderThread_);
    connect(&loaderThread_, &QThread::finished, loader_, &QObject::deleteLater);
    connect(this, &SamplePoolWidget::loadFileRequested, loader_, &SampleLoader::loadFile, Qt::QueuedConnection);
    connect(loader_, &SampleLoader::sampleLoaded, this, &SamplePoolWidget::addLoadedSample, Qt::QueuedConnection);
    loaderThread_.start();
}

SamplePoolWidget::~SamplePoolWidget()
{
    loaderThread_.quit();
    loaderThread_.wait();
}

Sample* SamplePoolWidget::selectedSample() const noexcept
{
    return selectedSample_;
}

std::vector<Sample*> SamplePoolWidget::allSamples() const
{
    std::vector<Sample*> result;
    result.reserve(samples_.size());
    for (const std::unique_ptr<Sample>& sample : samples_) {
        result.push_back(sample.get());
    }
    return result;
}

Sample* SamplePoolWidget::addRecordedSample(const QString& name, std::vector<float> data, uint32_t sampleRate, size_t frames)
{
    if (frames == 0U || data.size() < frames * 2U) {
        emit statusMessage(QStringLiteral("Recording was empty."));
        return nullptr;
    }

    auto sample = std::make_unique<Sample>();
    sample->data = std::move(data);
    sample->sampleRate = sampleRate == 0U ? 48000U : sampleRate;
    sample->originalSampleRate = sample->sampleRate;
    sample->frames = frames;
    sample->name = name;
    sample->createdInApp = true;
    sample->peakLevel = 0.0F;
    for (float value : sample->data) {
        sample->peakLevel = std::max(sample->peakLevel, std::abs(value));
    }
    sample->normalizationGain = sample->peakLevel > 0.000001F
        ? std::clamp(0.85F / sample->peakLevel, 0.10F, 4.0F)
        : 1.0F;
    sample->color = QColor::fromHsv((frames + name.size() * 31U) % 360U, 150, 215);
    sample->originalDurationSec = static_cast<double>(sample->frames) / static_cast<double>(sample->sampleRate);

    Sample* rawSample = sample.get();
    addSample(std::move(sample));
    selectSample(rawSample);
    emit statusMessage(QStringLiteral("Recorded %1").arg(rawSample->name));
    return rawSample;
}

Sample* SamplePoolWidget::addStateSample(const LoadedSample& loadedSample)
{
    if (!loadedSample.ok || loadedSample.frames == 0U || loadedSample.stereoData.size() < loadedSample.frames * 2U) {
        return nullptr;
    }

    auto sample = std::make_unique<Sample>();
    sample->data = loadedSample.stereoData;
    sample->sampleRate = loadedSample.sampleRate;
    sample->originalSampleRate = loadedSample.originalSampleRate;
    sample->frames = loadedSample.frames;
    sample->name = loadedSample.name;
    sample->sourceFilePath = loadedSample.sourceFilePath;
    sample->resamplerName = loadedSample.resamplerName;
    sample->createdInApp = false;
    sample->resampled = loadedSample.resampled;
    sample->normalizationGain = loadedSample.normalizationGain;
    sample->peakLevel = loadedSample.peakLevel;
    sample->color = loadedSample.color;

    if (targetSampleRate_ != 0U && sample->sampleRate != targetSampleRate_) {
        QString errorMessage;
        QString backendName;
        QString qualityName;
        size_t frames = sample->frames;
        uint32_t sampleRate = sample->sampleRate;
        if (SampleLoader::resampleStereo(
                &sample->data,
                &frames,
                &sampleRate,
                targetSampleRate_,
                resamplerBackend_,
                resamplerQuality_,
                &errorMessage,
                &backendName,
                &qualityName)) {
            sample->frames = frames;
            sample->sampleRate = sampleRate;
            sample->resamplerName = QStringLiteral("%1 (%2)").arg(backendName, qualityName);
            sample->resampled = sample->originalSampleRate != 0U && sample->sampleRate != sample->originalSampleRate;
        } else {
            emit statusMessage(QStringLiteral("Resample fallback failed for %1: %2").arg(sample->name, errorMessage));
        }
    }

    sample->originalDurationSec = sample->sampleRate == 0U
        ? 0.0
        : static_cast<double>(sample->frames) / static_cast<double>(sample->sampleRate);

    Sample* rawSample = sample.get();
    addSample(std::move(sample));
    return rawSample;
}

void SamplePoolWidget::clearSamples()
{
    stopMarkerPreview(false);
    markerEditTarget_ = MarkerEditTarget::None;
    undoStack_->clear();

    for (const std::unique_ptr<Sample>& sample : samples_) {
        thumbnailRequestIds_.erase(sample.get());
        emit sampleRemoved(sample.get());
    }

    for (SampleBlockWidget* block : blocks_) {
        block->deleteLater();
    }
    blocks_.clear();
    samples_.clear();
    selectedSample_ = nullptr;
    rebuildGrid();
    emit sampleSelected(nullptr);
}

void SamplePoolWidget::replaceSampleAudio(Sample* sample, std::vector<float> data, uint32_t sampleRate, size_t frames)
{
    if (sample == nullptr || frames == 0U || data.size() < frames * 2U) {
        emit statusMessage(QStringLiteral("Stretched audio was empty."));
        return;
    }

    if (sample->originalDurationSec <= 0.0 && sample->sampleRate != 0U) {
        sample->originalDurationSec = static_cast<double>(sample->frames) / static_cast<double>(sample->sampleRate);
    }

    sample->data = std::move(data);
    sample->sampleRate = sampleRate == 0U ? sample->sampleRate : sampleRate;
    sample->frames = frames;
    sample->resampled = sample->originalSampleRate != 0U && sample->sampleRate != sample->originalSampleRate;
    sample->startFrame = 0.0;
    sample->endFrame = 1.0;
    requestThumbnailRefresh(sample);
    if (sample == selectedSample_) {
        emit sampleSelected(sample);
    }
}

void SamplePoolWidget::refreshSample(Sample* sample)
{
    if (sample == nullptr) {
        return;
    }

    requestThumbnailRefresh(sample);
    if (sample == selectedSample_) {
        emit sampleSelected(sample);
    }
}

bool SamplePoolWidget::saveSampleToWav(Sample* sample, const QString& filePath, QString* errorMessage)
{
    if (sample == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("No sample.");
        }
        return false;
    }

    const bool ok = writeSampleToWav(*sample, filePath, errorMessage);
    if (ok) {
        sample->sourceFilePath = filePath;
        sample->createdInApp = true;
    }
    return ok;
}

void SamplePoolWidget::setTargetSampleRate(uint32_t sampleRate)
{
    targetSampleRate_ = sampleRate;
    QMetaObject::invokeMethod(
        loader_,
        "setTargetSampleRate",
        Qt::QueuedConnection,
        Q_ARG(uint32_t, sampleRate));
}

void SamplePoolWidget::setResamplerBackend(ResamplerBackend backend)
{
    resamplerBackend_ = backend;
    QMetaObject::invokeMethod(
        loader_,
        "setResamplerBackend",
        Qt::QueuedConnection,
        Q_ARG(int, static_cast<int>(backend)));
}

void SamplePoolWidget::setResamplerQuality(ResamplerQuality quality)
{
    resamplerQuality_ = quality;
    QMetaObject::invokeMethod(
        loader_,
        "setResamplerQuality",
        Qt::QueuedConnection,
        Q_ARG(int, static_cast<int>(quality)));
}

void SamplePoolWidget::setApplyNormalization(bool enabled) noexcept
{
    applyNormalization_ = enabled;
}

void SamplePoolWidget::resampleAllToTargetRate()
{
    if (targetSampleRate_ == 0U) {
        return;
    }

    for (const std::unique_ptr<Sample>& sample : samples_) {
        if (!sample || sample->sampleRate == targetSampleRate_ || sample->frames == 0U) {
            continue;
        }

        QString errorMessage;
        QString backendName;
        QString qualityName;
        size_t frames = sample->frames;
        uint32_t sampleRate = sample->sampleRate;
        if (!SampleLoader::resampleStereo(
                &sample->data,
                &frames,
                &sampleRate,
                targetSampleRate_,
                resamplerBackend_,
                resamplerQuality_,
                &errorMessage,
                &backendName,
                &qualityName)) {
            emit statusMessage(QStringLiteral("Resample failed for %1: %2").arg(sample->name, errorMessage));
            continue;
        }

        sample->frames = frames;
        sample->sampleRate = sampleRate;
        sample->resamplerName = QStringLiteral("%1 (%2)").arg(backendName, qualityName);
        sample->resampled = sample->originalSampleRate != 0U && sample->sampleRate != sample->originalSampleRate;
        sample->startFrame = 0.0;
        sample->endFrame = 1.0;
        requestThumbnailRefresh(sample.get());
        emit statusMessage(QStringLiteral("Resampled %1 with %2 %3").arg(sample->name, backendName, qualityName));
    }
    if (selectedSample_ != nullptr) {
        emit sampleSelected(selectedSample_);
    }
}

void SamplePoolWidget::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
        return;
    }

    QScrollArea::dragEnterEvent(event);
}

void SamplePoolWidget::dragMoveEvent(QDragMoveEvent* event)
{
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
        return;
    }

    QScrollArea::dragMoveEvent(event);
}

void SamplePoolWidget::dropEvent(QDropEvent* event)
{
    if (!event->mimeData()->hasUrls()) {
        QScrollArea::dropEvent(event);
        return;
    }

    int requestedLoads = 0;
    for (const QUrl& url : event->mimeData()->urls()) {
        if (!url.isLocalFile()) {
            continue;
        }

        emit loadFileRequested(url.toLocalFile());
        ++requestedLoads;
    }

    if (requestedLoads > 0) {
        emit statusMessage(QStringLiteral("Loading %1 sample%2")
            .arg(requestedLoads)
            .arg(requestedLoads == 1 ? QString() : QStringLiteral("s")));
    }

    event->acceptProposedAction();
}

void SamplePoolWidget::keyPressEvent(QKeyEvent* event)
{
    if (markerEditTarget_ != MarkerEditTarget::None) {
        if (event->isAutoRepeat()) {
            event->accept();
            return;
        }

        if (event->key() == Qt::Key_Left) {
            startMarkerPreview(-1, event->modifiers());
            event->accept();
            return;
        }

        if (event->key() == Qt::Key_Right) {
            startMarkerPreview(1, event->modifiers());
            event->accept();
            return;
        }

        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter || event->key() == Qt::Key_Escape) {
            finishMarkerEditMode();
            event->accept();
            return;
        }
    }

    if (event->key() == Qt::Key_Left && event->modifiers() == Qt::NoModifier) {
        navigateSelection(-1);
        event->accept();
        return;
    }

    if (event->key() == Qt::Key_Right && event->modifiers() == Qt::NoModifier) {
        navigateSelection(1);
        event->accept();
        return;
    }

    if (event->key() == Qt::Key_Up && event->modifiers() == Qt::NoModifier) {
        navigateSelection(-gridColumnCount_);
        event->accept();
        return;
    }

    if (event->key() == Qt::Key_Down && event->modifiers() == Qt::NoModifier) {
        navigateSelection(gridColumnCount_);
        event->accept();
        return;
    }

    if (event->key() == Qt::Key_Delete && event->modifiers() == Qt::NoModifier) {
        deleteSelectedSample();
        event->accept();
        return;
    }

    if (event->key() == Qt::Key_A && event->modifiers() == Qt::NoModifier) {
        relabelSelected('A');
        event->accept();
        return;
    }

    if (event->key() == Qt::Key_B && event->modifiers() == Qt::NoModifier) {
        relabelSelected('B');
        event->accept();
        return;
    }

    if (event->key() == Qt::Key_Space && event->modifiers() == Qt::NoModifier) {
        requestLayeredPlay();
        event->accept();
        return;
    }

    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && event->modifiers() == Qt::NoModifier) {
        appendLabelledSamples();
        event->accept();
        return;
    }

    if (event->key() == Qt::Key_S && event->modifiers() == Qt::ControlModifier) {
        enterMarkerEditMode(MarkerEditTarget::Start);
        event->accept();
        return;
    }

    if (event->key() == Qt::Key_E && event->modifiers() == Qt::ControlModifier) {
        enterMarkerEditMode(MarkerEditTarget::End);
        event->accept();
        return;
    }

    if (event->key() == Qt::Key_E && event->modifiers() == Qt::ShiftModifier) {
        exportInMemorySamplesToDirectory();
        event->accept();
        return;
    }

    if (event->key() == Qt::Key_T && event->modifiers() == Qt::ControlModifier) {
        autoTrimSelected();
        event->accept();
        return;
    }

    if (event->key() == Qt::Key_C && event->modifiers() == Qt::ControlModifier) {
        cloneSelectedTrimmedRegion();
        event->accept();
        return;
    }

    if (event->key() == Qt::Key_Z && event->modifiers() == Qt::ControlModifier) {
        undoStack_->undo();
        event->accept();
        return;
    }

    if (event->key() == Qt::Key_Y && event->modifiers() == Qt::ControlModifier) {
        undoStack_->redo();
        event->accept();
        return;
    }

    QScrollArea::keyPressEvent(event);
}

void SamplePoolWidget::keyReleaseEvent(QKeyEvent* event)
{
    if (event->isAutoRepeat()) {
        event->accept();
        return;
    }

    if (markerEditTarget_ != MarkerEditTarget::None
        && (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right)) {
        stopMarkerPreview(true);
        event->accept();
        return;
    }

    QScrollArea::keyReleaseEvent(event);
}

void SamplePoolWidget::resizeEvent(QResizeEvent* event)
{
    QScrollArea::resizeEvent(event);
    rebuildGrid();
}

void SamplePoolWidget::addLoadedSample(const LoadedSample& loadedSample)
{
    if (!loadedSample.ok) {
        emit statusMessage(QStringLiteral("Load failed for %1: %2").arg(loadedSample.name, loadedSample.errorMessage));
        return;
    }

    auto sample = std::make_unique<Sample>();
    sample->data = loadedSample.stereoData;
    sample->sampleRate = loadedSample.sampleRate;
    sample->originalSampleRate = loadedSample.originalSampleRate;
    sample->frames = loadedSample.frames;
    sample->name = loadedSample.name;
    sample->sourceFilePath = loadedSample.sourceFilePath;
    sample->resamplerName = loadedSample.resamplerName;
    sample->createdInApp = false;
    sample->resampled = loadedSample.resampled;
    sample->normalizationGain = loadedSample.normalizationGain;
    sample->peakLevel = loadedSample.peakLevel;
    sample->color = loadedSample.color;

    if (targetSampleRate_ != 0U && sample->sampleRate != targetSampleRate_) {
        QString errorMessage;
        QString backendName;
        QString qualityName;
        size_t frames = sample->frames;
        uint32_t sampleRate = sample->sampleRate;
        if (SampleLoader::resampleStereo(
                &sample->data,
                &frames,
                &sampleRate,
                targetSampleRate_,
                resamplerBackend_,
                resamplerQuality_,
                &errorMessage,
                &backendName,
                &qualityName)) {
            sample->frames = frames;
            sample->sampleRate = sampleRate;
            sample->resamplerName = QStringLiteral("%1 (%2)").arg(backendName, qualityName);
            sample->resampled = sample->originalSampleRate != 0U && sample->sampleRate != sample->originalSampleRate;
        } else {
            emit statusMessage(QStringLiteral("Resample fallback failed for %1: %2").arg(sample->name, errorMessage));
        }
    }

    sample->originalDurationSec = sample->sampleRate == 0U
        ? 0.0
        : static_cast<double>(sample->frames) / static_cast<double>(sample->sampleRate);
    Sample* rawSample = sample.get();
    addSample(std::move(sample));
    selectSample(rawSample);
    emit statusMessage(QStringLiteral("Loaded %1").arg(rawSample->name));
}

void SamplePoolWidget::selectSample(Sample* sample)
{
    selectedSample_ = sample;
    for (SampleBlockWidget* block : blocks_) {
        block->setSelected(block->sample() == sample);
    }

    setFocus(Qt::MouseFocusReason);
    emit sampleSelected(sample);
}

void SamplePoolWidget::handleThumbnailReady(quint64 sampleToken, quint64 requestId, const QImage& image)
{
    Sample* sample = reinterpret_cast<Sample*>(sampleToken);
    if (sample == nullptr) {
        return;
    }

    const auto it = thumbnailRequestIds_.find(sample);
    if (it == thumbnailRequestIds_.end() || it->second != requestId) {
        return;
    }

    sample->thumbnail = QPixmap::fromImage(image);
    refreshThumbnails();
    if (sample == selectedSample_) {
        emit sampleSelected(sample);
    }
}

void SamplePoolWidget::addSample(std::unique_ptr<Sample> sample)
{
    Sample* rawSample = sample.get();
    samples_.push_back(std::move(sample));

    auto* block = new SampleBlockWidget(rawSample, contentWidget_);
    connect(block, &SampleBlockWidget::selected, this, &SamplePoolWidget::selectSample);
    connect(block, &SampleBlockWidget::labelRequested, this, &SamplePoolWidget::labelSample);
    connect(block, &SampleBlockWidget::gainRequested, this, [this](Sample* sample) {
        if (sample == nullptr) {
            return;
        }

        bool ok = false;
        const int percent = QInputDialog::getInt(
            this,
            QStringLiteral("Sample Gain"),
            QStringLiteral("Gain percent:"),
            static_cast<int>(std::llround(sample->gain * 100.0F)),
            0,
            400,
            1,
            &ok);
        if (!ok) {
            return;
        }

        sample->gain = static_cast<float>(percent) / 100.0F;
        emit sampleSelected(sample);
        emit statusMessage(QStringLiteral("%1 gain: %2%").arg(sample->name).arg(percent));
    });
    connect(block, &SampleBlockWidget::exportRequested, this, &SamplePoolWidget::exportSample);
    blocks_.push_back(block);
    rebuildGrid();
    requestThumbnailRefresh(rawSample);
}

void SamplePoolWidget::rebuildGrid()
{
    if (gridLayout_ == nullptr) {
        return;
    }

    while (QLayoutItem* item = gridLayout_->takeAt(0)) {
        if (item->widget() != nullptr) {
            item->widget()->setParent(contentWidget_);
        }
        delete item;
    }

    const int viewportWidth = std::max(1, viewport()->width());
    const int columns = std::max(1, (viewportWidth - kBlockSpacing) / (kBlockSize + kBlockSpacing));
    gridColumnCount_ = columns;
    for (int index = 0; index < static_cast<int>(blocks_.size()); ++index) {
        gridLayout_->addWidget(blocks_[static_cast<size_t>(index)], index / columns, index % columns);
    }
}

void SamplePoolWidget::relabelSelected(char label)
{
    if (selectedSample_ == nullptr) {
        emit statusMessage(QStringLiteral("No selected sample to label."));
        return;
    }

    for (const std::unique_ptr<Sample>& sample : samples_) {
        if (sample->label == label) {
            sample->label = '\0';
        }
    }

    selectedSample_->label = label;
    refreshThumbnails();
    emit sampleSelected(selectedSample_);
    emit statusMessage(QStringLiteral("Labelled %1 as %2").arg(selectedSample_->name, QString(QChar::fromLatin1(label))));
}

void SamplePoolWidget::requestLayeredPlay()
{
    Sample* first = labelledSample('A');
    Sample* second = labelledSample('B');
    if (first == nullptr || second == nullptr) {
        emit statusMessage(QStringLiteral("Assign A and B labels before layered play."));
        return;
    }

    emit layeredPlayRequested(first, second);
    emit statusMessage(QStringLiteral("Layered play requested: %1 + %2").arg(first->name, second->name));
}

void SamplePoolWidget::appendLabelledSamples()
{
    Sample* first = labelledSample('A');
    Sample* second = labelledSample('B');
    if (first == nullptr || second == nullptr) {
        emit statusMessage(QStringLiteral("Assign A and B labels before append."));
        return;
    }

    std::unique_ptr<Sample> appended = makeAppendedSample(*first, *second);
    if (!appended) {
        return;
    }

    Sample* rawSample = appended.get();
    addSample(std::move(appended));
    selectSample(rawSample);
    emit appendedSampleCreated(rawSample);
    emit statusMessage(QStringLiteral("Created appended sample %1").arg(rawSample->name));
}

void SamplePoolWidget::enterMarkerEditMode(MarkerEditTarget target)
{
    if (selectedSample_ == nullptr || selectedSample_->frames == 0U) {
        emit statusMessage(QStringLiteral("No selected sample to edit."));
        return;
    }

    markerEditTarget_ = target;
    const QString targetName = target == MarkerEditTarget::Start ? QStringLiteral("start") : QStringLiteral("end");
    emit statusMessage(QStringLiteral("Editing %1 marker for %2").arg(targetName, selectedSample_->name));
}

void SamplePoolWidget::startMarkerPreview(int direction, Qt::KeyboardModifiers modifiers)
{
    if (selectedSample_ == nullptr || selectedSample_->frames == 0U || markerEditTarget_ == MarkerEditTarget::None) {
        return;
    }

    if (markerPreviewActive_) {
        return;
    }

    markerPreviewDirection_ = direction < 0 ? -1 : 1;
    markerPreviewSpeedRatio_ = previewSpeedForModifiers(modifiers);
    markerPreviewOriginalStart_ = selectedSample_->startFrame;
    markerPreviewOriginalEnd_ = selectedSample_->endFrame;
    markerPreviewOriginFrame_ = markerEditTarget_ == MarkerEditTarget::Start
        ? frameFromNormalized(*selectedSample_, selectedSample_->startFrame)
        : frameFromNormalized(*selectedSample_, selectedSample_->endFrame);
    if (markerPreviewOriginFrame_ >= selectedSample_->frames) {
        markerPreviewOriginFrame_ = selectedSample_->frames - 1U;
    }

    markerPreviewActive_ = true;
    markerPreviewClock_.restart();
    markerPreviewTimer_->start();
    emit markerPreviewStarted(selectedSample_, markerPreviewOriginFrame_, markerPreviewDirection_, markerPreviewSpeedRatio_);
    emit statusMessage(QStringLiteral("Auditioning %1 at %2x").arg(selectedSample_->name).arg(markerPreviewSpeedRatio_, 0, 'f', 1));
}

void SamplePoolWidget::updateMarkerPreviewPosition()
{
    if (!markerPreviewActive_ || selectedSample_ == nullptr || selectedSample_->frames == 0U) {
        return;
    }

    const double elapsedSeconds = static_cast<double>(markerPreviewClock_.elapsed()) / 1000.0;
    const double deltaFrames = elapsedSeconds
        * static_cast<double>(selectedSample_->sampleRate)
        * markerPreviewSpeedRatio_
        * static_cast<double>(markerPreviewDirection_);
    const auto unclampedFrame = static_cast<qint64>(std::llround(static_cast<double>(markerPreviewOriginFrame_) + deltaFrames));
    const size_t oldStartFrame = frameFromNormalized(*selectedSample_, selectedSample_->startFrame);
    const size_t oldEndFrame = frameFromNormalized(*selectedSample_, selectedSample_->endFrame);
    size_t newStartFrame = oldStartFrame;
    size_t newEndFrame = oldEndFrame;

    if (markerEditTarget_ == MarkerEditTarget::Start) {
        const qint64 maxStart = static_cast<qint64>(oldEndFrame > 0U ? oldEndFrame - 1U : 0U);
        newStartFrame = static_cast<size_t>(std::clamp<qint64>(unclampedFrame, 0, maxStart));
    } else {
        const qint64 minEnd = static_cast<qint64>(oldStartFrame + 1U);
        newEndFrame = static_cast<size_t>(std::clamp<qint64>(unclampedFrame, minEnd, static_cast<qint64>(selectedSample_->frames)));
    }

    applyTrimRange(
        selectedSample_,
        normalizedFromFrame(*selectedSample_, newStartFrame),
        normalizedFromFrame(*selectedSample_, newEndFrame));
}

void SamplePoolWidget::stopMarkerPreview(bool commit)
{
    if (!markerPreviewActive_) {
        return;
    }

    updateMarkerPreviewPosition();
    markerPreviewTimer_->stop();
    markerPreviewActive_ = false;
    emit markerPreviewStopped();

    if (commit && selectedSample_ != nullptr
        && (selectedSample_->startFrame != markerPreviewOriginalStart_
            || selectedSample_->endFrame != markerPreviewOriginalEnd_)) {
        const double finalStart = selectedSample_->startFrame;
        const double finalEnd = selectedSample_->endFrame;
        undoStack_->push(new TrimRangeCommand(
            this,
            selectedSample_,
            markerPreviewOriginalStart_,
            markerPreviewOriginalEnd_,
            finalStart,
            finalEnd,
            QStringLiteral("Audition sample marker")));
    }
}

void SamplePoolWidget::finishMarkerEditMode()
{
    stopMarkerPreview(true);
    markerEditTarget_ = MarkerEditTarget::None;
    emit statusMessage(QStringLiteral("Marker edit finished."));
}

void SamplePoolWidget::autoTrimSelected()
{
    if (selectedSample_ == nullptr || selectedSample_->frames == 0U) {
        emit statusMessage(QStringLiteral("No selected sample to auto-trim."));
        return;
    }

    const auto [currentStart, currentEnd] = trimmedFrameRange(*selectedSample_);
    std::optional<size_t> newStartFrame;
    std::optional<size_t> newEndFrame;

    for (size_t frame = currentStart; frame < currentEnd; ++frame) {
        if (framePeak(*selectedSample_, frame) >= kTrimThreshold) {
            newStartFrame = frame;
            break;
        }
    }

    for (size_t frame = currentEnd; frame > currentStart; --frame) {
        const size_t testFrame = frame - 1U;
        if (framePeak(*selectedSample_, testFrame) >= kTrimThreshold) {
            newEndFrame = testFrame + 1U;
            break;
        }
    }

    if (!newStartFrame.has_value() || !newEndFrame.has_value() || *newEndFrame <= *newStartFrame) {
        emit statusMessage(QStringLiteral("No auto-trim range found for %1").arg(selectedSample_->name));
        return;
    }

    const double newStart = normalizedFromFrame(*selectedSample_, *newStartFrame);
    const double newEnd = normalizedFromFrame(*selectedSample_, *newEndFrame);
    undoStack_->push(new TrimRangeCommand(
        this,
        selectedSample_,
        selectedSample_->startFrame,
        selectedSample_->endFrame,
        newStart,
        newEnd,
        QStringLiteral("Auto-trim sample")));

    emit statusMessage(QStringLiteral("Auto-trimmed %1").arg(selectedSample_->name));
}

void SamplePoolWidget::cloneSelectedTrimmedRegion()
{
    if (selectedSample_ == nullptr) {
        emit statusMessage(QStringLiteral("No selected sample to clone."));
        return;
    }

    TrimRangeDialog dialog(*selectedSample_, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    std::unique_ptr<Sample> clone = makeTrimmedClone(
        *selectedSample_,
        dialog.startFrameNormalized(),
        dialog.endFrameNormalized());
    if (!clone) {
        emit statusMessage(QStringLiteral("Selected trim range is empty."));
        return;
    }

    Sample* rawSample = clone.get();
    addSample(std::move(clone));
    selectSample(rawSample);
    emit statusMessage(QStringLiteral("Created trimmed sample %1").arg(rawSample->name));
}

void SamplePoolWidget::labelSample(Sample* sample)
{
    if (sample == nullptr) {
        emit statusMessage(QStringLiteral("No sample selected to label."));
        return;
    }

    bool ok = false;
    const QString label = QInputDialog::getText(
        this,
        QStringLiteral("Label Sample"),
        QStringLiteral("Label:"),
        QLineEdit::Normal,
        sample->userLabel.isEmpty() ? sample->name : sample->userLabel,
        &ok);
    if (!ok) {
        return;
    }

    sample->userLabel = label.trimmed();
    refreshThumbnails();
    emit sampleSelected(sample);
    emit statusMessage(sample->userLabel.isEmpty()
            ? QStringLiteral("Cleared label for %1").arg(sample->name)
            : QStringLiteral("Labelled %1 as %2").arg(sample->name, sample->userLabel));
}

void SamplePoolWidget::exportSample(Sample* sample)
{
    if (sample == nullptr) {
        emit statusMessage(QStringLiteral("No sample selected to export."));
        return;
    }

    const QString defaultName = safeFileBaseName(sample->name) + QStringLiteral(".wav");
    const QString filePath = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("Export Sample"),
        QDir::home().filePath(defaultName),
        QStringLiteral("WAV files (*.wav)"));
    if (filePath.isEmpty()) {
        return;
    }

    QString errorMessage;
    if (!writeSampleToWav(*sample, filePath, &errorMessage)) {
        emit statusMessage(QStringLiteral("Export failed: %1").arg(errorMessage));
        return;
    }

    if (sample->createdInApp) {
        sample->sourceFilePath = filePath;
    }
    emit statusMessage(QStringLiteral("Exported %1").arg(filePath));
}

void SamplePoolWidget::exportInMemorySamplesToDirectory()
{
    bool ok = false;
    const QString directoryPath = QInputDialog::getText(
        this,
        QStringLiteral("Save In-Memory Samples"),
        QStringLiteral("Save to:"),
        QLineEdit::Normal,
        QDir::homePath(),
        &ok);
    if (!ok || directoryPath.trimmed().isEmpty()) {
        return;
    }

    QDir directory(directoryPath.trimmed());
    if (!directory.exists() && !QDir().mkpath(directory.absolutePath())) {
        emit statusMessage(QStringLiteral("Could not create %1").arg(directory.absolutePath()));
        return;
    }

    int exportedCount = 0;
    for (std::unique_ptr<Sample>& sample : samples_) {
        if (!sample->sourceFilePath.isEmpty()) {
            continue;
        }

        QString baseName = safeFileBaseName(sample->name);
        QString filePath = directory.filePath(baseName + QStringLiteral(".wav"));
        int suffix = 2;
        while (QFileInfo::exists(filePath)) {
            filePath = directory.filePath(QStringLiteral("%1-%2.wav").arg(baseName).arg(suffix));
            ++suffix;
        }

        QString errorMessage;
        if (!writeSampleToWav(*sample, filePath, &errorMessage)) {
            emit statusMessage(QStringLiteral("Export failed for %1: %2").arg(sample->name, errorMessage));
            continue;
        }

        sample->sourceFilePath = filePath;
        ++exportedCount;
    }

    emit statusMessage(QStringLiteral("Exported %1 in-memory sample%2 to %3")
        .arg(exportedCount)
        .arg(exportedCount == 1 ? QString() : QStringLiteral("s"))
        .arg(directory.absolutePath()));
}

void SamplePoolWidget::refreshThumbnails()
{
    for (SampleBlockWidget* block : blocks_) {
        block->update();
    }
}

void SamplePoolWidget::requestThumbnailRefresh(Sample* sample)
{
    if (sample == nullptr) {
        return;
    }

    const quint64 requestId = nextThumbnailRequestId_++;
    thumbnailRequestIds_[sample] = requestId;
    auto* task = new ThumbnailTask(this, reinterpret_cast<quint64>(sample), requestId, makeThumbnailSnapshot(*sample));
    QThreadPool::globalInstance()->start(task);
}

void SamplePoolWidget::applyTrimRange(Sample* sample, double startFrame, double endFrame)
{
    if (sample == nullptr) {
        return;
    }

    sample->startFrame = std::clamp(startFrame, 0.0, 1.0);
    sample->endFrame = std::clamp(endFrame, sample->startFrame, 1.0);
    requestThumbnailRefresh(sample);
    if (sample == selectedSample_) {
        emit sampleSelected(sample);
    }
}

void SamplePoolWidget::navigateSelection(int delta)
{
    if (samples_.empty()) {
        selectSample(nullptr);
        return;
    }

    int currentIndex = 0;
    if (selectedSample_ != nullptr) {
        const auto it = std::find_if(samples_.cbegin(), samples_.cend(), [this](const std::unique_ptr<Sample>& sample) {
            return sample.get() == selectedSample_;
        });
        if (it != samples_.cend()) {
            currentIndex = static_cast<int>(std::distance(samples_.cbegin(), it));
        }
    }

    const int lastIndex = static_cast<int>(samples_.size()) - 1;
    const int nextIndex = std::clamp(currentIndex + delta, 0, lastIndex);
    selectSample(samples_[static_cast<size_t>(nextIndex)].get());
    if (nextIndex >= 0 && nextIndex < static_cast<int>(blocks_.size())) {
        ensureWidgetVisible(blocks_[static_cast<size_t>(nextIndex)]);
    }
}

void SamplePoolWidget::deleteSelectedSample()
{
    if (selectedSample_ == nullptr) {
        emit statusMessage(QStringLiteral("No selected sample to delete."));
        return;
    }

    Sample* sample = selectedSample_;
    if (sample->frames == 0U || sample->data.empty()) {
        if (!sample->sourceFilePath.isEmpty()) {
            QFile::remove(sample->sourceFilePath);
        }
        removeSample(sample);
        return;
    }

    if (!sample->createdInApp) {
        removeSample(sample);
        return;
    }

    QMessageBox dialog(this);
    dialog.setWindowTitle(QStringLiteral("Delete Sample"));
    dialog.setText(QStringLiteral("Do you want to remove the file from the samples area or delete it?"));
    QPushButton* removeButton = dialog.addButton(QStringLiteral("Remove"), QMessageBox::AcceptRole);
    QPushButton* deleteButton = dialog.addButton(QStringLiteral("Delete"), QMessageBox::DestructiveRole);
    QPushButton* cancelButton = dialog.addButton(QStringLiteral("Cancel"), QMessageBox::RejectRole);
    dialog.exec();

    QAbstractButton* clicked = dialog.clickedButton();
    if (clicked == nullptr || clicked == cancelButton) {
        return;
    }

    if (clicked == deleteButton && !sample->sourceFilePath.isEmpty()) {
        QFile::remove(sample->sourceFilePath);
    }

    if (clicked == removeButton || clicked == deleteButton) {
        removeSample(sample);
    }
}

void SamplePoolWidget::removeSample(Sample* sample)
{
    if (sample == nullptr) {
        return;
    }

    const auto sampleIt = std::find_if(samples_.begin(), samples_.end(), [sample](const std::unique_ptr<Sample>& ownedSample) {
        return ownedSample.get() == sample;
    });
    if (sampleIt == samples_.end()) {
        return;
    }

    const int removedIndex = static_cast<int>(std::distance(samples_.begin(), sampleIt));
    const auto blockIt = std::find_if(blocks_.begin(), blocks_.end(), [sample](const SampleBlockWidget* block) {
        return block->sample() == sample;
    });
    if (blockIt != blocks_.end()) {
        SampleBlockWidget* block = *blockIt;
        blocks_.erase(blockIt);
        block->deleteLater();
    }

    undoStack_->clear();
    thumbnailRequestIds_.erase(sample);
    emit sampleRemoved(sample);
    samples_.erase(sampleIt);

    if (samples_.empty()) {
        selectedSample_ = nullptr;
        rebuildGrid();
        emit sampleSelected(nullptr);
        emit statusMessage(QStringLiteral("Removed sample."));
        return;
    }

    const int nextIndex = std::min(removedIndex, static_cast<int>(samples_.size()) - 1);
    rebuildGrid();
    selectSample(samples_[static_cast<size_t>(nextIndex)].get());
    emit statusMessage(QStringLiteral("Removed sample."));
}

Sample* SamplePoolWidget::labelledSample(char label) const
{
    for (const std::unique_ptr<Sample>& sample : samples_) {
        if (sample->label == label) {
            return sample.get();
        }
    }

    return nullptr;
}

std::unique_ptr<Sample> SamplePoolWidget::makeAppendedSample(const Sample& first, const Sample& second)
{
    if (first.sampleRate != second.sampleRate) {
        emit statusMessage(QStringLiteral("Cannot append samples with different sample rates."));
        return nullptr;
    }

    const auto [firstStart, firstEnd] = trimmedFrameRange(first);
    const auto [secondStart, secondEnd] = trimmedFrameRange(second);
    if (firstEnd <= firstStart || secondEnd <= secondStart) {
        emit statusMessage(QStringLiteral("Cannot append empty trim ranges."));
        return nullptr;
    }

    auto result = std::make_unique<Sample>();
    result->sampleRate = first.sampleRate;
    result->name = QStringLiteral("%1 + %2").arg(first.name, second.name);
    result->createdInApp = true;
    result->color = first.color.lighter(115);
    result->frames = (firstEnd - firstStart) + (secondEnd - secondStart);
    result->data.reserve(result->frames * 2U);
    result->data.insert(result->data.end(), first.data.cbegin() + static_cast<std::ptrdiff_t>(firstStart * 2U), first.data.cbegin() + static_cast<std::ptrdiff_t>(firstEnd * 2U));
    result->data.insert(result->data.end(), second.data.cbegin() + static_cast<std::ptrdiff_t>(secondStart * 2U), second.data.cbegin() + static_cast<std::ptrdiff_t>(secondEnd * 2U));
    result->originalDurationSec = static_cast<double>(result->frames) / static_cast<double>(result->sampleRate);
    return result;
}

std::unique_ptr<Sample> SamplePoolWidget::makeTrimmedClone(const Sample& source) const
{
    return makeTrimmedClone(source, source.startFrame, source.endFrame);
}

std::unique_ptr<Sample> SamplePoolWidget::makeTrimmedClone(const Sample& source, double startFrame, double endFrame) const
{
    Sample rangeSource = source;
    rangeSource.startFrame = startFrame;
    rangeSource.endFrame = endFrame;
    const auto [start, end] = trimmedFrameRange(rangeSource);
    if (end <= start) {
        return nullptr;
    }

    auto result = std::make_unique<Sample>();
    result->sampleRate = source.sampleRate;
    result->name = QStringLiteral("%1 trim").arg(source.name);
    result->createdInApp = true;
    result->color = source.color.lighter(120);
    result->frames = end - start;
    result->data.insert(result->data.end(), source.data.cbegin() + static_cast<std::ptrdiff_t>(start * 2U), source.data.cbegin() + static_cast<std::ptrdiff_t>(end * 2U));
    result->originalDurationSec = static_cast<double>(result->frames) / static_cast<double>(result->sampleRate);
    return result;
}

bool SamplePoolWidget::writeSampleToWav(const Sample& sample, const QString& filePath, QString* errorMessage)
{
    if (sample.sampleRate == 0U || sample.frames == 0U || sample.data.size() < sample.frames * 2U) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Sample is empty.");
        }
        return false;
    }

    const auto [startFrame, endFrame] = trimmedFrameRange(sample);
    if (endFrame <= startFrame) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Trim range is empty.");
        }
        return false;
    }

    SF_INFO info {};
    info.samplerate = static_cast<int>(sample.sampleRate);
    info.channels = 2;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;

    SNDFILE* file = sf_open(filePath.toLocal8Bit().constData(), SFM_WRITE, &info);
    if (file == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = QString::fromLocal8Bit(sf_strerror(nullptr));
        }
        return false;
    }

    const float* dataStart = sample.data.data() + (startFrame * 2U);
    const sf_count_t framesToWrite = static_cast<sf_count_t>(endFrame - startFrame);
    const sf_count_t framesWritten = sf_writef_float(file, dataStart, framesToWrite);
    sf_close(file);

    if (framesWritten != framesToWrite) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Could not write all sample frames.");
        }
        return false;
    }

    return true;
}

QString SamplePoolWidget::safeFileBaseName(const QString& name)
{
    QString baseName = QFileInfo(name).completeBaseName();
    if (baseName.isEmpty()) {
        baseName = QStringLiteral("sample");
    }

    for (QChar& ch : baseName) {
        if (!ch.isLetterOrNumber() && ch != QLatin1Char('-') && ch != QLatin1Char('_')) {
            ch = QLatin1Char('_');
        }
    }

    return baseName;
}

double SamplePoolWidget::previewSpeedForModifiers(Qt::KeyboardModifiers modifiers)
{
    if (modifiers & Qt::ShiftModifier) {
        return 4.0;
    }

    if (modifiers & Qt::AltModifier) {
        return 0.5;
    }

    return 1.0;
}

#include "SamplePoolWidget.moc"
