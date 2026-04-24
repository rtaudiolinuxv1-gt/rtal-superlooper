#include "MainWindow.h"

#include "Lv2Host.h"
#include "OptimizationLog.h"
#include "PedalBar.h"
#include "PianoWidget.h"
#include "SamplePoolWidget.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include <QAction>
#include <QAbstractItemView>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDial>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFile>
#include <QFileSystemModel>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFrame>
#include <QFormLayout>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QHash>
#include <QMap>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QKeySequence>
#include <QPainter>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QScreen>
#include <QScrollArea>
#include <QSpinBox>
#include <QRegularExpression>
#include <QSplitter>
#include <QStatusBar>
#include <QStringList>
#include <QTimer>
#include <QTreeView>
#include <QTextEdit>
#include <QVariant>
#include <QVBoxLayout>
#include <QWidget>

namespace {
constexpr double kMaxRecordSeconds = 60.0;
constexpr qint64 kTapHoldThresholdMs = 220;

QLabel* makePanelLabel(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setAlignment(Qt::AlignCenter);
    label->setFrameShape(QFrame::StyledPanel);
    label->setMinimumHeight(56);
    return label;
}

QWidget* pairWidget(const QString& labelText, QWidget* field, QWidget* parent)
{
    auto* widget = new QWidget(parent);
    auto* layout = new QHBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    auto* label = new QLabel(labelText, widget);
    label->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
    layout->addWidget(label);
    layout->addWidget(field);
    return widget;
}

QString safeStateFileBaseName(const QString& fileName)
{
    QString baseName = QFileInfo(fileName).completeBaseName();
    if (baseName.isEmpty()) {
        baseName = QStringLiteral("superlooper-state");
    }

    for (QChar& ch : baseName) {
        if (!ch.isLetterOrNumber() && ch != QLatin1Char('-') && ch != QLatin1Char('_')) {
            ch = QLatin1Char('_');
        }
    }

    return baseName;
}

QString uniqueWavPath(const QDir& directory, const QString& baseName)
{
    QString cleanBaseName = safeStateFileBaseName(baseName);
    QString path = directory.filePath(cleanBaseName + QStringLiteral(".wav"));
    int suffix = 2;
    while (QFileInfo::exists(path)) {
        path = directory.filePath(QStringLiteral("%1-%2.wav").arg(cleanBaseName).arg(suffix));
        ++suffix;
    }
    return path;
}

QVariant settingsValueWithLegacyFallback(
    const QSettings& settings,
    const QSettings& legacySettings,
    const QString& key,
    const QVariant& defaultValue = {})
{
    return settings.contains(key) ? settings.value(key, defaultValue) : legacySettings.value(key, defaultValue);
}

QString loopModeName(KeyLoopMode mode)
{
    return mode == KeyLoopMode::NoLoop ? QStringLiteral("No Loop") : QStringLiteral("Loop");
}

QString beatDetectionScopeName(MainWindow::BeatDetectionScope scope)
{
    switch (scope) {
    case MainWindow::BeatDetectionScope::Beginning:
        return QStringLiteral("Detect from beginning");
    case MainWindow::BeatDetectionScope::End:
        return QStringLiteral("Detect from end");
    case MainWindow::BeatDetectionScope::BeginningAndEndAverage:
        return QStringLiteral("Detect from average of beginning and end");
    case MainWindow::BeatDetectionScope::EntireSample:
        return QStringLiteral("Detect from entire sample");
    }
    return QStringLiteral("Detect from beginning");
}

MainWindow::BeatDetectionScope beatDetectionScopeFromName(const QString& name)
{
    if (name == QStringLiteral("Detect from end")) {
        return MainWindow::BeatDetectionScope::End;
    }
    if (name == QStringLiteral("Detect from average of beginning and end")) {
        return MainWindow::BeatDetectionScope::BeginningAndEndAverage;
    }
    if (name == QStringLiteral("Detect from entire sample")) {
        return MainWindow::BeatDetectionScope::EntireSample;
    }
    return MainWindow::BeatDetectionScope::Beginning;
}

QString beatDetectionMergePolicyName(MainWindow::BeatDetectionMergePolicy policy)
{
    switch (policy) {
    case MainWindow::BeatDetectionMergePolicy::PreferHigherConfidence:
        return QStringLiteral("Prefer higher confidence");
    case MainWindow::BeatDetectionMergePolicy::Average:
        return QStringLiteral("Average windows");
    }
    return QStringLiteral("Prefer higher confidence");
}

MainWindow::BeatDetectionMergePolicy beatDetectionMergePolicyFromName(const QString& name)
{
    if (name == QStringLiteral("Average windows")) {
        return MainWindow::BeatDetectionMergePolicy::Average;
    }
    return MainWindow::BeatDetectionMergePolicy::PreferHigherConfidence;
}

int midiNoteFromToken(const QString& token)
{
    bool ok = false;
    const int directValue = token.toInt(&ok);
    if (ok) {
        return std::clamp(directValue, 0, 127);
    }

    QString upper = token.trimmed().toUpper();
    if (upper.isEmpty()) {
        return -1;
    }

    static const QMap<QString, int> semitones {
        { QStringLiteral("C"), 0 }, { QStringLiteral("C#"), 1 }, { QStringLiteral("DB"), 1 },
        { QStringLiteral("D"), 2 }, { QStringLiteral("D#"), 3 }, { QStringLiteral("EB"), 3 },
        { QStringLiteral("E"), 4 }, { QStringLiteral("F"), 5 }, { QStringLiteral("F#"), 6 },
        { QStringLiteral("GB"), 6 }, { QStringLiteral("G"), 7 }, { QStringLiteral("G#"), 8 },
        { QStringLiteral("AB"), 8 }, { QStringLiteral("A"), 9 }, { QStringLiteral("A#"), 10 },
        { QStringLiteral("BB"), 10 }, { QStringLiteral("B"), 11 }
    };

    const int noteNameLength = upper.size() > 1 && (upper[1] == QLatin1Char('#') || upper[1] == QLatin1Char('B')) ? 2 : 1;
    const QString noteName = upper.left(noteNameLength);
    const int semitone = semitones.value(noteName, -1);
    if (semitone < 0) {
        return -1;
    }

    const int octave = upper.mid(noteNameLength).toInt(&ok);
    if (!ok) {
        return -1;
    }

    return (octave + 1) * 12 + semitone;
}

QHash<QString, QString> parseSfzOpcodes(const QString& text)
{
    QHash<QString, QString> opcodes;
    int index = 0;
    while (index < text.size()) {
        while (index < text.size() && text.at(index).isSpace()) {
            ++index;
        }
        if (index >= text.size()) {
            break;
        }

        const int keyStart = index;
        while (index < text.size() && !text.at(index).isSpace() && text.at(index) != QLatin1Char('=')) {
            ++index;
        }
        if (index >= text.size() || text.at(index) != QLatin1Char('=')) {
            while (index < text.size() && !text.at(index).isSpace()) {
                ++index;
            }
            continue;
        }

        const QString key = text.mid(keyStart, index - keyStart).trimmed().toLower();
        ++index;

        QString value;
        if (index < text.size() && text.at(index) == QLatin1Char('"')) {
            ++index;
            const int valueStart = index;
            while (index < text.size() && text.at(index) != QLatin1Char('"')) {
                ++index;
            }
            value = text.mid(valueStart, index - valueStart);
            if (index < text.size() && text.at(index) == QLatin1Char('"')) {
                ++index;
            }
        } else {
            QString buffer;
            while (index < text.size() && !text.at(index).isSpace()) {
                if (text.at(index) == QLatin1Char('\\') && index + 1 < text.size()) {
                    const QChar next = text.at(index + 1);
                    if (next.isSpace() || next == QLatin1Char('\\')) {
                        buffer.append(next);
                        index += 2;
                        continue;
                    }
                }
                buffer.append(text.at(index));
                ++index;
            }
            value = buffer;
        }

        if (!key.isEmpty()) {
            opcodes.insert(key, value.trimmed());
        }
    }

    return opcodes;
}

QString stripSfzComments(const QString& line)
{
    const int commentIndex = line.indexOf(QStringLiteral("//"));
    return commentIndex >= 0 ? line.left(commentIndex) : line;
}

QString resolveSfzPath(const QDir& baseDir, const QString& defaultPath, const QString& sampleToken)
{
    if (sampleToken.isEmpty()) {
        return QString();
    }

    QString relativePath = sampleToken;
    relativePath.replace(QLatin1Char('\\'), QDir::separator());
    if (!defaultPath.isEmpty() && QDir::isRelativePath(relativePath)) {
        QString prefix = defaultPath;
        prefix.replace(QLatin1Char('\\'), QDir::separator());
        relativePath = QDir(prefix).filePath(relativePath);
    }

    return QFileInfo(relativePath).isAbsolute()
        ? QDir::cleanPath(relativePath)
        : QDir::cleanPath(baseDir.filePath(relativePath));
}

KeyLoopMode keyLoopModeFromSfz(const QString& loopMode)
{
    const QString normalized = loopMode.trimmed().toLower();
    if (normalized == QStringLiteral("no_loop")
        || normalized == QStringLiteral("one_shot")
        || normalized == QStringLiteral("loop_off")) {
        return KeyLoopMode::NoLoop;
    }

    return KeyLoopMode::Loop;
}

QString sfzLoopModeValue(const QHash<QString, QString>& opcodes)
{
    if (opcodes.contains(QStringLiteral("loop_mode"))) {
        return opcodes.value(QStringLiteral("loop_mode"));
    }
    return opcodes.value(QStringLiteral("loopmode"));
}

double sfzCombinedGainDecibels(const QHash<QString, QString>& opcodes)
{
    const double volumeDb = opcodes.value(QStringLiteral("volume"), QStringLiteral("0")).toDouble();
    const double gainDb = opcodes.value(QStringLiteral("gain"), QStringLiteral("0")).toDouble();
    return volumeDb + gainDb;
}

double sfzPitchKeytrackPercent(const QHash<QString, QString>& opcodes)
{
    return std::clamp(
        opcodes.value(QStringLiteral("pitch_keytrack"), QStringLiteral("100")).toDouble(),
        0.0,
        1200.0);
}

std::pair<double, double> sfzTrimRangeFromOpcodes(const QHash<QString, QString>& opcodes, size_t sampleFrames)
{
    if (sampleFrames == 0U) {
        return { 0.0, 1.0 };
    }

    const bool hasOffset = opcodes.contains(QStringLiteral("offset"));
    const bool hasEnd = opcodes.contains(QStringLiteral("end"));
    const bool hasLoopStart = opcodes.contains(QStringLiteral("loop_start"));
    const bool hasLoopEnd = opcodes.contains(QStringLiteral("loop_end"));

    const double offsetValue = hasOffset
        ? opcodes.value(QStringLiteral("offset")).toDouble()
        : (hasLoopStart ? opcodes.value(QStringLiteral("loop_start")).toDouble() : 0.0);
    const double endValue = hasEnd
        ? opcodes.value(QStringLiteral("end")).toDouble()
        : (hasLoopEnd ? opcodes.value(QStringLiteral("loop_end")).toDouble() : static_cast<double>(sampleFrames));

    const size_t offsetFrames = static_cast<size_t>(std::clamp(offsetValue, 0.0, static_cast<double>(sampleFrames)));
    const double endFrames = std::clamp(endValue, static_cast<double>(offsetFrames), static_cast<double>(sampleFrames));
    const double trimStart = std::clamp(static_cast<double>(offsetFrames) / static_cast<double>(sampleFrames), 0.0, 1.0);
    const double trimEnd = std::clamp(endFrames / static_cast<double>(sampleFrames), trimStart, 1.0);
    return { trimStart, trimEnd };
}

int percentFromDecibels(double decibels)
{
    return std::clamp(
        static_cast<int>(std::llround(std::pow(10.0, decibels / 20.0) * 100.0)),
        0,
        400);
}

QString selectLv2PluginUri(QWidget* parent, const QString& title, const QString& currentUri)
{
    if (!Lv2Host::isAvailable()) {
        QMessageBox::warning(parent, title, Lv2Host::unavailableReason());
        return currentUri;
    }

    const std::vector<Lv2PluginInfo> plugins = Lv2Host::availableStereoPlugins();
    QStringList items;
    items << QStringLiteral("None");
    int currentIndex = 0;
    for (size_t index = 0; index < plugins.size(); ++index) {
        const QString label = QStringLiteral("%1 [%2]").arg(plugins[index].name, plugins[index].uri);
        items << label;
        if (plugins[index].uri == currentUri) {
            currentIndex = static_cast<int>(index) + 1;
        }
    }

    bool ok = false;
    const QString selected = QInputDialog::getItem(
        parent,
        title,
        QStringLiteral("LV2 plugin:"),
        items,
        currentIndex,
        false,
        &ok);
    if (!ok) {
        return currentUri;
    }

    if (selected == QStringLiteral("None")) {
        return QString();
    }

    const int uriStart = selected.lastIndexOf(QStringLiteral(" ["));
    const int uriEnd = selected.endsWith(QLatin1Char(']')) ? selected.size() - 1 : -1;
    if (uriStart >= 0 && uriEnd > uriStart + 2) {
        return selected.mid(uriStart + 2, uriEnd - (uriStart + 2));
    }

    return currentUri;
}

QJsonArray jsonArrayFromFloatVector(const std::vector<float>& values)
{
    QJsonArray array;
    for (float value : values) {
        array.push_back(static_cast<double>(value));
    }
    return array;
}

std::vector<float> floatVectorFromJsonArray(const QJsonArray& array)
{
    std::vector<float> values;
    values.reserve(static_cast<size_t>(array.size()));
    for (const QJsonValue& value : array) {
        values.push_back(static_cast<float>(value.toDouble()));
    }
    return values;
}

QVariantList variantListFromFloatVector(const std::vector<float>& values)
{
    QVariantList list;
    for (float value : values) {
        list.push_back(value);
    }
    return list;
}

std::vector<float> floatVectorFromVariant(const QVariant& variant)
{
    std::vector<float> values;
    const QVariantList list = variant.toList();
    values.reserve(static_cast<size_t>(list.size()));
    for (const QVariant& value : list) {
        values.push_back(value.toFloat());
    }
    return values;
}
}

class DragLineOverlay final : public QWidget
{
public:
    explicit DragLineOverlay(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setAttribute(Qt::WA_TranslucentBackground);
        hide();
        if (parent != nullptr) {
            parent->installEventFilter(this);
            setGeometry(parent->rect());
        }
    }

    void setLine(const QPoint& globalStart, const QPoint& globalEnd, bool active)
    {
        active_ = active;
        lastGlobalStart_ = globalStart;
        lastGlobalEnd_ = globalEnd;
        if (!active_) {
            hide();
            update();
            return;
        }

        updateLocalPoints();
        raise();
        show();
        update();
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched == parentWidget()
            && (event->type() == QEvent::Resize || event->type() == QEvent::Move)) {
            setGeometry(parentWidget()->rect());
            updateLocalPoints();
        }

        return QWidget::eventFilter(watched, event);
    }

    void paintEvent(QPaintEvent* event) override
    {
        Q_UNUSED(event)

        if (!active_) {
            return;
        }

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(QColor(255, 232, 140), 4, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(start_, end_);
        painter.setBrush(QColor(255, 232, 140));
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(end_, 6, 6);
    }

private:
    void updateLocalPoints()
    {
        start_ = mapFromGlobal(lastGlobalStart_);
        end_ = mapFromGlobal(lastGlobalEnd_);
    }

    QPoint start_;
    QPoint end_;
    QPoint lastGlobalStart_;
    QPoint lastGlobalEnd_;
    bool active_ = false;
};

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    qRegisterMetaType<StretchRequest>("StretchRequest");
    qRegisterMetaType<StretchResult>("StretchResult");
    qRegisterMetaType<RootTimingRequest>("RootTimingRequest");
    qRegisterMetaType<RootTimingResult>("RootTimingResult");

    setWindowTitle(QStringLiteral("RtAudioLinux's SuperLooper"));
    const QRect availableGeometry = QGuiApplication::primaryScreen() != nullptr
        ? QGuiApplication::primaryScreen()->availableGeometry()
        : QRect(0, 0, 1180, 720);
    const int initialWidth = std::clamp(availableGeometry.width() - 80, 900, 1380);
    const int initialHeight = std::clamp(availableGeometry.height() - 90, 560, 900);
    resize(initialWidth, initialHeight);
    setMinimumSize(760, 500);
    appClock_.start();
    perKeyVolumePercent_.fill(100);
    perKeyPanPercent_.fill(0);
    perKeyMuted_.fill(false);
    perKeySolo_.fill(false);
    perKeyGroup_.fill(0);
    perKeyAttackMs_.fill(0.0);
    perKeyReleaseMs_.fill(0.0);
    perKeyStaccatoPercent_.fill(0);
    perKeyLoopMode_.fill(KeyLoopMode::Loop);
    perKeyVirtualStaccatoEnabled_.fill(false);
    perKeySelfMixEnabled_.fill(false);
    perKeyTrimStart_.fill(0.0);
    perKeyTrimEnd_.fill(1.0);
    perKeyPlaybackRate_.fill(1.0);
    for (size_t noteIndex = 0; noteIndex < perKeyLv2Enabled_.size(); ++noteIndex) {
        perKeyLv2Enabled_[noteIndex].fill(true);
        perKeyLv2PluginUris_[noteIndex].fill(QString());
        for (std::vector<float>& values : perKeyLv2ParameterValues_[noteIndex]) {
            values.clear();
        }
    }
    masterLv2Enabled_.fill(true);
    masterLv2PluginUris_.fill(QString());
    for (std::vector<float>& values : masterLv2ParameterValues_) {
        values.clear();
    }
    groupGainPercent_.fill(100);
    groupPanPercent_.fill(0);
    groupMuted_.fill(false);
    groupSolo_.fill(false);

    createCentralLayout();
    loadSettings();

    createMenus();
    updateModeLabel();

    recordingPollTimer_ = new QTimer(this);
    recordingPollTimer_->setInterval(30);
    connect(recordingPollTimer_, &QTimer::timeout, this, &MainWindow::pollFinishedRecordings);
    recordingPollTimer_->start();

    rubberBandWorker_ = new RubberBandWorker;
    rubberBandWorker_->moveToThread(&rubberBandThread_);
    connect(&rubberBandThread_, &QThread::finished, rubberBandWorker_, &QObject::deleteLater);
    connect(this, &MainWindow::destroyed, &rubberBandThread_, &QThread::quit);
    connect(rubberBandWorker_, &RubberBandWorker::stretchFinished, this, &MainWindow::handleStretchFinished, Qt::QueuedConnection);
    rubberBandThread_.start();

    rootTimingWorker_ = new RootTimingWorker;
    rootTimingWorker_->moveToThread(&rootTimingThread_);
    connect(&rootTimingThread_, &QThread::finished, rootTimingWorker_, &QObject::deleteLater);
    connect(this, &MainWindow::destroyed, &rootTimingThread_, &QThread::quit);
    connect(rootTimingWorker_, &RootTimingWorker::analysisFinished, this, &MainWindow::handleRootTimingFinished, Qt::QueuedConnection);
    rootTimingThread_.start();

    statusBar()->showMessage(QStringLiteral("Disconnected from JACK"));
    updateBottomStatus();
}

MainWindow::~MainWindow()
{
    saveSettings();
    audioEngine_.disconnectFromJack();
    rubberBandThread_.quit();
    rubberBandThread_.wait();
    rootTimingThread_.quit();
    rootTimingThread_.wait();
}

void MainWindow::connectToJack()
{
    QString errorMessage;
    if (!audioEngine_.connectToJack(&errorMessage)) {
        statusBar()->showMessage(errorMessage);
        return;
    }

    samplePoolWidget_->setTargetSampleRate(audioEngine_.audioSampleRate());
    audioEngine_.stopAllPlayback();
    samplePoolWidget_->resampleAllToTargetRate();
    applyMixerSettings();
    connectAction_->setEnabled(false);
    statusBar()->showMessage(QStringLiteral("Connected to JACK"));
    updateBottomStatus();
}

void MainWindow::connectMidiInput()
{
    QString errorMessage;
    const QStringList portNames = MidiInput::inputPortNames(&errorMessage);
    if (portNames.isEmpty()) {
        statusBar()->showMessage(errorMessage.isEmpty()
                ? QStringLiteral("No MIDI input ports are available.")
                : QStringLiteral("MIDI connect failed: %1").arg(errorMessage));
        return;
    }

    bool ok = false;
    const QString selectedPort = QInputDialog::getItem(
        this,
        QStringLiteral("Connect MIDI Input"),
        QStringLiteral("MIDI device:"),
        portNames,
        0,
        false,
        &ok);
    if (!ok || selectedPort.isEmpty()) {
        return;
    }

    const int selectedIndex = portNames.indexOf(selectedPort);
    if (selectedIndex < 0 || !midiInput_.connectInputPort(static_cast<unsigned int>(selectedIndex), &errorMessage)) {
        statusBar()->showMessage(QStringLiteral("MIDI connect failed: %1").arg(errorMessage));
        return;
    }

    connectMidiAction_->setEnabled(false);
    statusBar()->showMessage(QStringLiteral("Connected MIDI input: %1").arg(selectedPort));
}

void MainWindow::saveState()
{
    QString statePath = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("Save SuperLooper State"),
        QDir::home().filePath(QStringLiteral("superlooper-state.json")),
        QStringLiteral("SuperLooper state (*.json)"));
    if (statePath.isEmpty()) {
        return;
    }
    if (QFileInfo(statePath).suffix().isEmpty()) {
        statePath += QStringLiteral(".json");
    }

    const QFileInfo stateInfo(statePath);
    QDir stateDir = stateInfo.absoluteDir();
    QDir sampleDir(stateDir.filePath(safeStateFileBaseName(stateInfo.fileName()) + QStringLiteral("_samples")));
    if (!sampleDir.exists() && !stateDir.mkpath(sampleDir.dirName())) {
        statusBar()->showMessage(QStringLiteral("Could not create sample folder %1").arg(sampleDir.absolutePath()));
        return;
    }

    std::vector<Sample*> samples = samplePoolWidget_->allSamples();
    std::unordered_map<Sample*, int> sampleIndexes;
    sampleIndexes.reserve(samples.size());

    QJsonArray sampleArray;
    for (int index = 0; index < static_cast<int>(samples.size()); ++index) {
        Sample* sample = samples[static_cast<size_t>(index)];
        sampleIndexes[sample] = index;

        if (sample->sourceFilePath.isEmpty() || !QFileInfo::exists(sample->sourceFilePath)) {
            const QString wavPath = uniqueWavPath(sampleDir, sample->userLabel.isEmpty() ? sample->name : sample->userLabel);
            QString errorMessage;
            if (!samplePoolWidget_->saveSampleToWav(sample, wavPath, &errorMessage)) {
                QMessageBox::warning(this, QStringLiteral("Save State"), QStringLiteral("Could not save %1: %2").arg(sample->name, errorMessage));
                return;
            }
        }

        QJsonObject sampleObject;
        sampleObject.insert(QStringLiteral("name"), sample->name);
        sampleObject.insert(QStringLiteral("userLabel"), sample->userLabel);
        sampleObject.insert(QStringLiteral("sourceFilePath"), sample->sourceFilePath);
        sampleObject.insert(QStringLiteral("relativePath"), stateDir.relativeFilePath(sample->sourceFilePath));
        sampleObject.insert(QStringLiteral("startFrame"), sample->startFrame);
        sampleObject.insert(QStringLiteral("endFrame"), sample->endFrame);
        sampleObject.insert(QStringLiteral("isRoot"), sample->isRoot);
        sampleObject.insert(QStringLiteral("syncToRoot"), sample->syncToRoot);
        sampleObject.insert(QStringLiteral("createdInApp"), sample->createdInApp);
        sampleObject.insert(QStringLiteral("gain"), sample->gain);
        sampleObject.insert(QStringLiteral("normalizationGain"), sample->normalizationGain);
        sampleObject.insert(QStringLiteral("peakLevel"), sample->peakLevel);
        sampleObject.insert(QStringLiteral("originalDurationSec"), sample->originalDurationSec);
        sampleObject.insert(QStringLiteral("originalSampleRate"), static_cast<int>(sample->originalSampleRate));
        sampleObject.insert(QStringLiteral("abLabel"), sample->label == '\0' ? QString() : QString(QChar::fromLatin1(sample->label)));
        sampleObject.insert(QStringLiteral("color"), sample->color.name(QColor::HexArgb));
        sampleArray.push_back(sampleObject);
    }

    QJsonArray keyArray;
    for (int noteIndex = 0; noteIndex < PianoWidget::kKeyCount; ++noteIndex) {
        QJsonObject keyObject;
        keyObject.insert(QStringLiteral("noteIndex"), noteIndex);
        keyObject.insert(QStringLiteral("autoMode"), perKeyAutoMode_[static_cast<size_t>(noteIndex)]);
        keyObject.insert(QStringLiteral("volumePercent"), perKeyVolumePercent_[static_cast<size_t>(noteIndex)]);
        keyObject.insert(QStringLiteral("panPercent"), perKeyPanPercent_[static_cast<size_t>(noteIndex)]);
        keyObject.insert(QStringLiteral("muted"), perKeyMuted_[static_cast<size_t>(noteIndex)]);
        keyObject.insert(QStringLiteral("solo"), perKeySolo_[static_cast<size_t>(noteIndex)]);
        keyObject.insert(QStringLiteral("group"), perKeyGroup_[static_cast<size_t>(noteIndex)]);
        keyObject.insert(QStringLiteral("attackMs"), perKeyAttackMs_[static_cast<size_t>(noteIndex)]);
        keyObject.insert(QStringLiteral("releaseMs"), perKeyReleaseMs_[static_cast<size_t>(noteIndex)]);
        keyObject.insert(QStringLiteral("staccatoPercent"), perKeyStaccatoPercent_[static_cast<size_t>(noteIndex)]);
        keyObject.insert(QStringLiteral("loopMode"), loopModeName(perKeyLoopMode_[static_cast<size_t>(noteIndex)]));
        keyObject.insert(QStringLiteral("virtualStaccatoEnabled"), perKeyVirtualStaccatoEnabled_[static_cast<size_t>(noteIndex)]);
        keyObject.insert(QStringLiteral("selfMixEnabled"), perKeySelfMixEnabled_[static_cast<size_t>(noteIndex)]);
        keyObject.insert(QStringLiteral("trimStart"), perKeyTrimStart_[static_cast<size_t>(noteIndex)]);
        keyObject.insert(QStringLiteral("trimEnd"), perKeyTrimEnd_[static_cast<size_t>(noteIndex)]);
        keyObject.insert(QStringLiteral("playbackRate"), perKeyPlaybackRate_[static_cast<size_t>(noteIndex)]);
        keyObject.insert(QStringLiteral("lv2PluginUri"), perKeyLv2PluginUris_[static_cast<size_t>(noteIndex)][0]);
        keyObject.insert(QStringLiteral("lv2ParameterValues"), jsonArrayFromFloatVector(perKeyLv2ParameterValues_[static_cast<size_t>(noteIndex)][0]));
        QJsonArray lv2SlotsArray;
        for (size_t slotIndex = 0; slotIndex < AudioEngine::kLv2ChainSize; ++slotIndex) {
            QJsonObject slotObject;
            slotObject.insert(QStringLiteral("slotIndex"), static_cast<int>(slotIndex));
            slotObject.insert(QStringLiteral("uri"), perKeyLv2PluginUris_[static_cast<size_t>(noteIndex)][slotIndex]);
            slotObject.insert(QStringLiteral("enabled"), perKeyLv2Enabled_[static_cast<size_t>(noteIndex)][slotIndex]);
            slotObject.insert(QStringLiteral("parameterValues"), jsonArrayFromFloatVector(perKeyLv2ParameterValues_[static_cast<size_t>(noteIndex)][slotIndex]));
            lv2SlotsArray.push_back(slotObject);
        }
        keyObject.insert(QStringLiteral("lv2Slots"), lv2SlotsArray);
        int sampleIndex = -1;
        if (Sample* sample = assignedSamples_[static_cast<size_t>(noteIndex)]; sample != nullptr) {
            const auto it = sampleIndexes.find(sample);
            if (it != sampleIndexes.end()) {
                sampleIndex = it->second;
            }
        }
        keyObject.insert(QStringLiteral("sampleIndex"), sampleIndex);
        keyArray.push_back(keyObject);
    }

    QJsonObject root;
    root.insert(QStringLiteral("format"), QStringLiteral("SuperLooperState"));
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("loopDurationSec"), loopDurationSpinBox_->value());
    root.insert(QStringLiteral("barsPerRoot"), barsPerRootSpinBox_->value());
    root.insert(QStringLiteral("recordingMethod"), static_cast<int>(recordingMethod_));
    root.insert(QStringLiteral("globalLoopDurationSec"), globalLoopDurationSec_);
    root.insert(QStringLiteral("tempoBpm"), tempoBpm_);
    root.insert(QStringLiteral("resampler"), SampleLoader::resamplerBackendName(resamplerBackend_));
    root.insert(QStringLiteral("resamplerQuality"), SampleLoader::resamplerQualityName(resamplerQuality_));
    root.insert(QStringLiteral("beatDetectionScope"), beatDetectionScopeName(beatDetectionScope_));
    root.insert(QStringLiteral("beatDetectionMergePolicy"), beatDetectionMergePolicyName(beatDetectionMergePolicy_));
    root.insert(QStringLiteral("beatDetectionStartLengthSeconds"), beatDetectionStartLengthSeconds_);
    root.insert(QStringLiteral("beatDetectionEndLengthSeconds"), beatDetectionEndLengthSeconds_);
    root.insert(QStringLiteral("sampleNormalizationEnabled"), sampleNormalizationEnabled_);
    root.insert(QStringLiteral("limiterEnabled"), limiterEnabled_);
    root.insert(QStringLiteral("fadeTimeMs"), fadeTimeMs_);
    root.insert(QStringLiteral("loopCrossfadeEnabled"), loopCrossfadeEnabled_);
    root.insert(QStringLiteral("loopCrossfadeMs"), loopCrossfadeMs_);
    root.insert(QStringLiteral("masterGainPercent"), masterGainPercent_);
    root.insert(QStringLiteral("masterLv2PluginUri"), masterLv2PluginUris_[0]);
    root.insert(QStringLiteral("masterLv2ParameterValues"), jsonArrayFromFloatVector(masterLv2ParameterValues_[0]));
    QJsonArray masterLv2SlotsArray;
    for (size_t slotIndex = 0; slotIndex < AudioEngine::kLv2ChainSize; ++slotIndex) {
        QJsonObject slotObject;
        slotObject.insert(QStringLiteral("slotIndex"), static_cast<int>(slotIndex));
        slotObject.insert(QStringLiteral("uri"), masterLv2PluginUris_[slotIndex]);
        slotObject.insert(QStringLiteral("enabled"), masterLv2Enabled_[slotIndex]);
        slotObject.insert(QStringLiteral("parameterValues"), jsonArrayFromFloatVector(masterLv2ParameterValues_[slotIndex]));
        masterLv2SlotsArray.push_back(slotObject);
    }
    root.insert(QStringLiteral("masterLv2Slots"), masterLv2SlotsArray);
    QJsonArray groupArray;
    for (int group = 0; group < 4; ++group) {
        QJsonObject groupObject;
        groupObject.insert(QStringLiteral("index"), group);
        groupObject.insert(QStringLiteral("gainPercent"), groupGainPercent_[static_cast<size_t>(group)]);
        groupObject.insert(QStringLiteral("panPercent"), groupPanPercent_[static_cast<size_t>(group)]);
        groupObject.insert(QStringLiteral("muted"), groupMuted_[static_cast<size_t>(group)]);
        groupObject.insert(QStringLiteral("solo"), groupSolo_[static_cast<size_t>(group)]);
        groupArray.push_back(groupObject);
    }
    root.insert(QStringLiteral("groups"), groupArray);
    root.insert(QStringLiteral("samples"), sampleArray);
    root.insert(QStringLiteral("keys"), keyArray);

    QFile file(statePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        statusBar()->showMessage(QStringLiteral("Could not write %1").arg(statePath));
        return;
    }

    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    statusBar()->showMessage(QStringLiteral("Saved state %1").arg(statePath));
}

void MainWindow::loadState()
{
    const QString statePath = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("Load SuperLooper State"),
        QDir::homePath(),
        QStringLiteral("SuperLooper state (*.json)"));
    if (statePath.isEmpty()) {
        return;
    }

    QFile file(statePath);
    if (!file.open(QIODevice::ReadOnly)) {
        statusBar()->showMessage(QStringLiteral("Could not open %1").arg(statePath));
        return;
    }

    QJsonParseError parseError {};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        QMessageBox::warning(this, QStringLiteral("Load State"), QStringLiteral("Invalid JSON: %1").arg(parseError.errorString()));
        return;
    }

    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("format")).toString() != QStringLiteral("JackLooperState")
        && root.value(QStringLiteral("format")).toString() != QStringLiteral("SuperLooperState")) {
        QMessageBox::warning(this, QStringLiteral("Load State"), QStringLiteral("This is not a SuperLooper state file."));
        return;
    }

    audioEngine_.stopAllPlayback();
    audioEngine_.stopPreview();
    samplePoolWidget_->clearSamples();
    assignedSamples_.fill(nullptr);
    stretchRequestIds_.clear();
    rootSample_ = nullptr;
    globalLoopDurationSec_ = 0.0;
    tempoBpm_ = 0.0;
    selectedNoteIndex_ = -1;
    pianoWidget_->setSelectedNoteIndex(-1);

    const QFileInfo stateInfo(statePath);
    const QDir stateDir = stateInfo.absoluteDir();
    std::vector<Sample*> loadedSamples;
    resamplerBackend_ = SampleLoader::resamplerBackendFromName(root.value(QStringLiteral("resampler")).toString(QStringLiteral("Automatic")));
    resamplerQuality_ = SampleLoader::resamplerQualityFromName(root.value(QStringLiteral("resamplerQuality")).toString(QStringLiteral("Highest Available")));
    beatDetectionScope_ = beatDetectionScopeFromName(root.value(QStringLiteral("beatDetectionScope")).toString(QStringLiteral("Detect from beginning")));
    beatDetectionMergePolicy_ = beatDetectionMergePolicyFromName(root.value(QStringLiteral("beatDetectionMergePolicy")).toString(QStringLiteral("Prefer higher confidence")));
    const double legacyWindowSeconds = std::clamp(root.value(QStringLiteral("beatDetectionWindowSeconds")).toDouble(20.0), 5.0, 120.0);
    beatDetectionStartLengthSeconds_ = std::clamp(root.value(QStringLiteral("beatDetectionStartLengthSeconds")).toDouble(legacyWindowSeconds), 5.0, 120.0);
    beatDetectionEndLengthSeconds_ = std::clamp(root.value(QStringLiteral("beatDetectionEndLengthSeconds")).toDouble(legacyWindowSeconds), 5.0, 120.0);
    sampleNormalizationEnabled_ = root.value(QStringLiteral("sampleNormalizationEnabled")).toBool(false);
    limiterEnabled_ = root.value(QStringLiteral("limiterEnabled")).toBool(true);
    fadeTimeMs_ = root.value(QStringLiteral("fadeTimeMs")).toDouble(5.0);
    loopCrossfadeEnabled_ = root.value(QStringLiteral("loopCrossfadeEnabled")).toBool(false);
    loopCrossfadeMs_ = root.value(QStringLiteral("loopCrossfadeMs")).toDouble(10.0);
    masterGainPercent_ = std::clamp(root.value(QStringLiteral("masterGainPercent")).toInt(100), 0, 200);
    masterLv2PluginUris_.fill(QString());
    masterLv2Enabled_.fill(true);
    for (std::vector<float>& values : masterLv2ParameterValues_) {
        values.clear();
    }
    if (root.contains(QStringLiteral("masterLv2Slots"))) {
        for (const QJsonValue& value : root.value(QStringLiteral("masterLv2Slots")).toArray()) {
            const QJsonObject slotObject = value.toObject();
            const int slotIndex = slotObject.value(QStringLiteral("slotIndex")).toInt(-1);
            if (slotIndex < 0 || slotIndex >= static_cast<int>(AudioEngine::kLv2ChainSize)) {
                continue;
            }
            masterLv2PluginUris_[static_cast<size_t>(slotIndex)] = slotObject.value(QStringLiteral("uri")).toString();
            masterLv2Enabled_[static_cast<size_t>(slotIndex)] = slotObject.value(QStringLiteral("enabled")).toBool(true);
            masterLv2ParameterValues_[static_cast<size_t>(slotIndex)] =
                floatVectorFromJsonArray(slotObject.value(QStringLiteral("parameterValues")).toArray());
        }
    } else {
        masterLv2PluginUris_[0] = root.value(QStringLiteral("masterLv2PluginUri")).toString();
        masterLv2ParameterValues_[0] = floatVectorFromJsonArray(root.value(QStringLiteral("masterLv2ParameterValues")).toArray());
    }
    masterGainDial_->setValue(masterGainPercent_);

    groupGainPercent_.fill(100);
    groupPanPercent_.fill(0);
    groupMuted_.fill(false);
    groupSolo_.fill(false);
    for (const QJsonValue& value : root.value(QStringLiteral("groups")).toArray()) {
        const QJsonObject groupObject = value.toObject();
        const int group = groupObject.value(QStringLiteral("index")).toInt(-1);
        if (group < 0 || group >= 4) {
            continue;
        }
        groupGainPercent_[static_cast<size_t>(group)] = std::clamp(groupObject.value(QStringLiteral("gainPercent")).toInt(100), 0, 200);
        groupPanPercent_[static_cast<size_t>(group)] = std::clamp(groupObject.value(QStringLiteral("panPercent")).toInt(0), -100, 100);
        groupMuted_[static_cast<size_t>(group)] = groupObject.value(QStringLiteral("muted")).toBool(false);
        groupSolo_[static_cast<size_t>(group)] = groupObject.value(QStringLiteral("solo")).toBool(false);
    }

    const QJsonArray sampleArray = root.value(QStringLiteral("samples")).toArray();
    loadedSamples.reserve(static_cast<size_t>(sampleArray.size()));
    for (const QJsonValue& value : sampleArray) {
        const QJsonObject sampleObject = value.toObject();
        QString sourcePath = sampleObject.value(QStringLiteral("sourceFilePath")).toString();
        const QString relativePath = sampleObject.value(QStringLiteral("relativePath")).toString();
        if (!relativePath.isEmpty() && QFileInfo::exists(stateDir.filePath(relativePath))) {
            sourcePath = stateDir.filePath(relativePath);
        }

        LoadedSample loadedSample = SampleLoader::loadAudioFile(
            sourcePath,
            audioEngine_.isConnected() ? audioEngine_.audioSampleRate() : 0U,
            resamplerBackend_,
            resamplerQuality_);
        if (!loadedSample.ok) {
            QMessageBox::warning(this, QStringLiteral("Load State"), QStringLiteral("Could not load %1: %2").arg(sourcePath, loadedSample.errorMessage));
            loadedSamples.push_back(nullptr);
            continue;
        }

        Sample* sample = samplePoolWidget_->addStateSample(loadedSample);
        if (sample == nullptr) {
            loadedSamples.push_back(nullptr);
            continue;
        }

        sample->name = sampleObject.value(QStringLiteral("name")).toString(sample->name);
        sample->userLabel = sampleObject.value(QStringLiteral("userLabel")).toString();
        sample->sourceFilePath = sourcePath;
        sample->startFrame = std::clamp(sampleObject.value(QStringLiteral("startFrame")).toDouble(0.0), 0.0, 1.0);
        sample->endFrame = std::clamp(sampleObject.value(QStringLiteral("endFrame")).toDouble(1.0), sample->startFrame, 1.0);
        sample->isRoot = sampleObject.value(QStringLiteral("isRoot")).toBool(false);
        sample->syncToRoot = sampleObject.value(QStringLiteral("syncToRoot")).toBool(false);
        sample->createdInApp = sampleObject.value(QStringLiteral("createdInApp")).toBool(false);
        sample->gain = static_cast<float>(sampleObject.value(QStringLiteral("gain")).toDouble(1.0));
        sample->normalizationGain = static_cast<float>(sampleObject.value(QStringLiteral("normalizationGain")).toDouble(sample->normalizationGain));
        sample->peakLevel = static_cast<float>(sampleObject.value(QStringLiteral("peakLevel")).toDouble(sample->peakLevel));
        sample->originalDurationSec = sampleObject.value(QStringLiteral("originalDurationSec")).toDouble(sample->originalDurationSec);
        sample->originalSampleRate = static_cast<uint32_t>(sampleObject.value(QStringLiteral("originalSampleRate")).toInt(static_cast<int>(sample->originalSampleRate)));
        sample->resampled = sample->originalSampleRate != 0U && sample->sampleRate != sample->originalSampleRate;
        sample->timingAnalysisState = TimingAnalysisState::Unanalyzed;
        sample->timingSyncState = TimingSyncState::Idle;
        sample->detectedTempoBpm = 0.0;
        sample->syncedFromTempoBpm = 0.0;
        sample->currentTempoBpm = 0.0;
        sample->timingError.clear();
        sample->hasTimingAnalysis = false;
        sample->hasBeatGrid = false;
        sample->analyzedRootStartFrame = 0.0;
        sample->analyzedActiveEndFrame = 0.0;
        sample->analyzedBeatPeriodFrames = 0.0;
        sample->analyzedTempoBpm = 0.0;
        sample->analyzedTempoConfidence = 0.0;
        sample->analyzedBeatCount = 0;
        sample->analyzedOnsetCount = 0;
        const QString abLabel = sampleObject.value(QStringLiteral("abLabel")).toString();
        sample->label = abLabel.isEmpty() ? '\0' : abLabel.at(0).toLatin1();
        const QColor color(sampleObject.value(QStringLiteral("color")).toString());
        if (color.isValid()) {
            sample->color = color;
        }
        if (sample->isRoot) {
            rootSample_ = sample;
        }
        samplePoolWidget_->refreshSample(sample);
        loadedSamples.push_back(sample);
    }

    loopDurationSpinBox_->setValue(root.value(QStringLiteral("loopDurationSec")).toDouble(4.0));
    barsPerRootSpinBox_->setValue(root.value(QStringLiteral("barsPerRoot")).toInt(1));
    recordingMethodComboBox_->setCurrentIndex(root.value(QStringLiteral("recordingMethod")).toInt(0) == 1 ? 1 : 0);
    setRecordingMethod(recordingMethodComboBox_->currentIndex());

    perKeyAutoMode_.fill(false);
    perKeyVolumePercent_.fill(100);
    perKeyPanPercent_.fill(0);
    perKeyMuted_.fill(false);
    perKeySolo_.fill(false);
    perKeyGroup_.fill(0);
    perKeyAttackMs_.fill(0.0);
    perKeyReleaseMs_.fill(0.0);
    perKeyStaccatoPercent_.fill(0);
    perKeyLoopMode_.fill(KeyLoopMode::Loop);
    perKeyVirtualStaccatoEnabled_.fill(false);
    perKeySelfMixEnabled_.fill(false);
    perKeyTrimStart_.fill(0.0);
    perKeyTrimEnd_.fill(1.0);
    perKeyPlaybackRate_.fill(1.0);
    for (size_t noteIndex = 0; noteIndex < perKeyLv2PluginUris_.size(); ++noteIndex) {
        perKeyLv2PluginUris_[noteIndex].fill(QString());
        perKeyLv2Enabled_[noteIndex].fill(true);
        for (std::vector<float>& values : perKeyLv2ParameterValues_[noteIndex]) {
            values.clear();
        }
    }
    const QJsonArray keyArray = root.value(QStringLiteral("keys")).toArray();
    for (const QJsonValue& value : keyArray) {
        const QJsonObject keyObject = value.toObject();
        const int noteIndex = keyObject.value(QStringLiteral("noteIndex")).toInt(-1);
        if (noteIndex < 0 || noteIndex >= PianoWidget::kKeyCount) {
            continue;
        }

        perKeyAutoMode_[static_cast<size_t>(noteIndex)] = keyObject.value(QStringLiteral("autoMode")).toBool(false);
        perKeyVolumePercent_[static_cast<size_t>(noteIndex)] = std::clamp(keyObject.value(QStringLiteral("volumePercent")).toInt(100), 0, 200);
        perKeyPanPercent_[static_cast<size_t>(noteIndex)] = std::clamp(keyObject.value(QStringLiteral("panPercent")).toInt(0), -100, 100);
        perKeyMuted_[static_cast<size_t>(noteIndex)] = keyObject.value(QStringLiteral("muted")).toBool(false);
        perKeySolo_[static_cast<size_t>(noteIndex)] = keyObject.value(QStringLiteral("solo")).toBool(false);
        perKeyGroup_[static_cast<size_t>(noteIndex)] = std::clamp(keyObject.value(QStringLiteral("group")).toInt(0), 0, 3);
        perKeyAttackMs_[static_cast<size_t>(noteIndex)] = std::clamp(keyObject.value(QStringLiteral("attackMs")).toDouble(0.0), 0.0, 5000.0);
        perKeyReleaseMs_[static_cast<size_t>(noteIndex)] = std::clamp(keyObject.value(QStringLiteral("releaseMs")).toDouble(0.0), 0.0, 5000.0);
        perKeyStaccatoPercent_[static_cast<size_t>(noteIndex)] = std::clamp(keyObject.value(QStringLiteral("staccatoPercent")).toInt(0), 0, 100);
        perKeyLoopMode_[static_cast<size_t>(noteIndex)] =
            keyObject.value(QStringLiteral("loopMode")).toString() == QStringLiteral("No Loop")
            ? KeyLoopMode::NoLoop
            : KeyLoopMode::Loop;
        perKeyVirtualStaccatoEnabled_[static_cast<size_t>(noteIndex)] =
            keyObject.contains(QStringLiteral("virtualStaccatoEnabled"))
            ? keyObject.value(QStringLiteral("virtualStaccatoEnabled")).toBool(false)
            : (perKeyLoopMode_[static_cast<size_t>(noteIndex)] == KeyLoopMode::NoLoop);
        perKeySelfMixEnabled_[static_cast<size_t>(noteIndex)] = keyObject.value(QStringLiteral("selfMixEnabled")).toBool(false);
        perKeyTrimStart_[static_cast<size_t>(noteIndex)] = std::clamp(keyObject.value(QStringLiteral("trimStart")).toDouble(0.0), 0.0, 1.0);
        perKeyTrimEnd_[static_cast<size_t>(noteIndex)] = std::clamp(keyObject.value(QStringLiteral("trimEnd")).toDouble(1.0), perKeyTrimStart_[static_cast<size_t>(noteIndex)], 1.0);
        perKeyPlaybackRate_[static_cast<size_t>(noteIndex)] = std::clamp(keyObject.value(QStringLiteral("playbackRate")).toDouble(1.0), 0.0625, 16.0);
        if (keyObject.contains(QStringLiteral("lv2Slots"))) {
            for (const QJsonValue& slotValue : keyObject.value(QStringLiteral("lv2Slots")).toArray()) {
                const QJsonObject slotObject = slotValue.toObject();
                const int slotIndex = slotObject.value(QStringLiteral("slotIndex")).toInt(-1);
                if (slotIndex < 0 || slotIndex >= static_cast<int>(AudioEngine::kLv2ChainSize)) {
                    continue;
                }
                perKeyLv2PluginUris_[static_cast<size_t>(noteIndex)][static_cast<size_t>(slotIndex)] =
                    slotObject.value(QStringLiteral("uri")).toString();
                perKeyLv2Enabled_[static_cast<size_t>(noteIndex)][static_cast<size_t>(slotIndex)] =
                    slotObject.value(QStringLiteral("enabled")).toBool(true);
                perKeyLv2ParameterValues_[static_cast<size_t>(noteIndex)][static_cast<size_t>(slotIndex)] =
                    floatVectorFromJsonArray(slotObject.value(QStringLiteral("parameterValues")).toArray());
            }
        } else {
            perKeyLv2PluginUris_[static_cast<size_t>(noteIndex)][0] = keyObject.value(QStringLiteral("lv2PluginUri")).toString();
            perKeyLv2ParameterValues_[static_cast<size_t>(noteIndex)][0] = floatVectorFromJsonArray(
                keyObject.value(QStringLiteral("lv2ParameterValues")).toArray());
        }

        const int sampleIndex = keyObject.value(QStringLiteral("sampleIndex")).toInt(-1);
        if (sampleIndex >= 0 && sampleIndex < static_cast<int>(loadedSamples.size())) {
            if (Sample* sample = loadedSamples[static_cast<size_t>(sampleIndex)]; sample != nullptr) {
                assignSampleToKey(noteIndex, sample);
            }
        }
    }

    if (rootSample_ != nullptr) {
        setRootSample(rootSample_);
    } else {
        updateTempoFromRoot();
    }
    for (Sample* sample : samplePoolWidget_->allSamples()) {
        if (sample != nullptr && sample != rootSample_ && sample->syncToRoot) {
            updateSampleTimingStateForSync(sample);
        }
    }
    applyMixerSettings();
    updateSampleFlagControls(samplePoolWidget_->selectedSample());
    updateKeySettingsControls(selectedNoteIndex_);
    updateBottomStatus();
    statusBar()->showMessage(QStringLiteral("Loaded state %1").arg(statePath));
}

void MainWindow::showHelp()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("SuperLooper Help"));
    dialog.resize(std::min(width() - 80, 820), std::min(height() - 80, 620));
    auto* layout = new QVBoxLayout(&dialog);
    auto* textEdit = new QTextEdit(&dialog);
    textEdit->setReadOnly(true);
    textEdit->setLineWrapMode(QTextEdit::WidgetWidth);
    textEdit->setText(QStringLiteral(
            "Core workflow\n"
            "- File -> Save State writes a JSON state and any memory-only WAVs beside it.\n"
            "- File -> Load State restores samples, key assignments, trims, labels, and settings.\n"
            "- File -> Import SFZ reads key-mapped SFZ instruments, including inherited defaults, trim offsets, pan, volume, and pitch-center/tuning.\n"
            "- File -> Export SFZ writes current key assignments as an SFZ plus any in-memory WAVs.\n"
            "- File -> Export Current Looper State as SFZ Bundle writes a folder with an SFZ and rendered sample copies for the current assigned keys.\n"
            "- Audio -> Connect to JACK before recording or playback. SuperLooper will try to auto-connect physical capture/playback ports.\n"
            "- Left pedal, grave key (`), or MIDI CC67 cycles Normal -> Record -> Playback -> Edit.\n"
            "- Drag audio files from the browser into the Sample Pool.\n"
            "- Drag a sample block from the pool onto a piano key to assign it.\n\n"
            "Piano input\n"
            "- Mouse: click piano keys.\n"
            "- QWERTY: Z S X D C V G B H N J M, then Q 2 W 3 E R 5 T 6 Y 7 U I.\n"
            "- [ and ] shift the keyboard octave.\n"
            "- MIDI notes 21-108 map to the 88 piano keys.\n\n"
            "Recording and playback\n"
            "- Record mode: tap a key for the Loop Duration; hold a key to record until release.\n"
            "- Select a key and toggle Auto to use onset/silence recording for that key.\n"
            "- Recording method Key Presses / Automatic records from piano key press/release.\n"
            "- Recording method Armed / Middle Pedal arms a key first; the middle pedal then starts/releases that armed recording.\n"
            "- Playback mode: tap an assigned key to loop it; tap again to stop at the loop end.\n\n"
            "Edit mode\n"
            "- Edit mode is reached by cycling the left pedal after Playback.\n"
            "- Click a piano key, press a QWERTY-mapped note, or send MIDI note-on to select a key for editing.\n"
            "- Volume, pan, mute, solo, self-mix, group, and Play/Pause control the selected piano key only in Edit mode.\n\n"
            "Mixer and audio quality\n"
            "- The master gain dial controls final output level; the Peak meter shows current output level.\n"
            "- Settings -> Audio and Mixer Settings controls resampling backend, fades, limiter, loop crossfade, sample normalization, and group buses.\n"
            "- Settings -> Master LV2 Chain configures up to five stereo LV2 effects across the full output mix.\n"
            "- Right-click a piano key to configure up to five stereo LV2 effects for that key only.\n"
            "- The final soft limiter prevents stacked loops from clipping harshly.\n"
            "- Imported files are resampled to the JACK sample rate when JACK is connected.\n\n"
            "Emergency stop\n"
            "- Press Emergency Stop to immediately silence all active sample playback and scrub preview audio.\n\n"
            "Sample pool keys\n"
            "- Arrow keys move selection through sample blocks.\n"
            "- Delete removes imported samples from the pool; app-created samples ask Remove / Delete / Cancel.\n"
            "- Right-click a sample and choose Export Sample to save it as a WAV.\n"
            "- Right-click a sample and choose Label Sample to set its display label.\n"
            "- Right-click a sample and choose Sample Gain to set its per-sample gain.\n"
            "- Shift+E saves all in-memory samples to a typed directory.\n"
            "- A / B label the selected sample.\n"
            "- Space plays A+B layered.\n"
            "- Enter appends B to A as a new sample.\n"
            "- Ctrl+T auto-trims start/end by threshold.\n"
            "- Ctrl+S edits the start marker; hold Left/Right to audition backward/forward at 1x, Shift for 4x, Alt for 0.5x, Enter/Esc exits.\n"
            "- Ctrl+E edits the end marker; hold Left/Right to audition backward/forward at 1x, Shift for 4x, Alt for 0.5x, Enter/Esc exits.\n"
            "- Ctrl+C opens the trim dialog; Left/Right switches start/end, Up/Down refines, Enter creates a trimmed sample.\n"
            "- Ctrl+Z / Ctrl+Y undo/redo marker edits.\n\n"
            "Sync\n"
            "- Root marks the timing source. Tempo is bars * 240 / root duration.\n"
            "- Sync stretches selected non-root samples to the root duration with RubberBand.\n\n"
            "Diagnostics\n"
            "- Help -> Optimization Summary shows the current optimization log.\n\n"
            "MIDI pedals\n"
            "- CC66 sostenuto triggers the app middle pedal for Armed / Middle Pedal recording.\n"
            "- CC67 soft pedal triggers the app left pedal / mode toggle.\n"
            "- CC64 sustain is not mapped to the app left pedal."));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(textEdit);
    layout->addWidget(buttons);
    dialog.exec();
}

void MainWindow::createMenus()
{
    QMenu* fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    QAction* saveStateAction = fileMenu->addAction(QStringLiteral("Save State..."));
    connect(saveStateAction, &QAction::triggered, this, &MainWindow::saveState);
    QAction* loadStateAction = fileMenu->addAction(QStringLiteral("Load State..."));
    connect(loadStateAction, &QAction::triggered, this, &MainWindow::loadState);
    fileMenu->addSeparator();
    QAction* importSfzAction = fileMenu->addAction(QStringLiteral("Import SFZ..."));
    connect(importSfzAction, &QAction::triggered, this, &MainWindow::importSfz);
    QAction* exportSfzAction = fileMenu->addAction(QStringLiteral("Export SFZ..."));
    connect(exportSfzAction, &QAction::triggered, this, &MainWindow::exportSfz);
    QAction* exportBundleAction = fileMenu->addAction(QStringLiteral("Export Current Looper State as SFZ Bundle..."));
    connect(exportBundleAction, &QAction::triggered, this, [this]() {
        const QString directory = QFileDialog::getExistingDirectory(
            this,
            QStringLiteral("Export Current Looper State as SFZ Bundle"),
            QDir::homePath());
        if (directory.isEmpty()) {
            return;
        }

        QString sfzPath;
        QString errorMessage;
        if (!exportAssignedKeymapAsSfzBundle(directory, QStringLiteral("superlooper-keymap"), &sfzPath, &errorMessage)) {
            QMessageBox::warning(this, QStringLiteral("Export SFZ Bundle"), errorMessage);
            return;
        }

        statusBar()->showMessage(QStringLiteral("Exported SFZ bundle to %1").arg(sfzPath));
    });

    QMenu* audioMenu = menuBar()->addMenu(QStringLiteral("&Audio"));
    connectAction_ = audioMenu->addAction(QStringLiteral("Connect to JACK"));
    connect(connectAction_, &QAction::triggered, this, &MainWindow::connectToJack);

    QMenu* midiMenu = menuBar()->addMenu(QStringLiteral("&MIDI"));
    connectMidiAction_ = midiMenu->addAction(QStringLiteral("Connect MIDI Input..."));
    connect(connectMidiAction_, &QAction::triggered, this, &MainWindow::connectMidiInput);

    QMenu* settingsMenu = menuBar()->addMenu(QStringLiteral("&Settings"));
    QAction* audioSettingsAction = settingsMenu->addAction(QStringLiteral("Audio and Mixer Settings..."));
    connect(audioSettingsAction, &QAction::triggered, this, &MainWindow::showSettingsDialog);
    QAction* masterLv2Action = settingsMenu->addAction(QStringLiteral("Master LV2 Chain..."));
    connect(masterLv2Action, &QAction::triggered, this, [this]() {
        if (!editLv2Chain(
                QStringLiteral("Master LV2 Chain"),
                &masterLv2PluginUris_,
                &masterLv2Enabled_,
                &masterLv2ParameterValues_)) {
            return;
        }

        for (size_t slotIndex = 0; slotIndex < AudioEngine::kLv2ChainSize; ++slotIndex) {
            QString errorMessage;
            if (!audioEngine_.setMasterLv2SlotUri(slotIndex, masterLv2PluginUris_[slotIndex], &errorMessage)) {
                QMessageBox::warning(this, QStringLiteral("Master LV2 Chain"), errorMessage);
                return;
            }
            audioEngine_.setMasterLv2SlotEnabled(slotIndex, masterLv2Enabled_[slotIndex]);
            audioEngine_.setMasterLv2SlotParameterValues(slotIndex, masterLv2ParameterValues_[slotIndex]);
        }
        saveSettings();
        statusBar()->showMessage(QStringLiteral("Updated master LV2 chain."));
    });

    QMenu* helpMenu = menuBar()->addMenu(QStringLiteral("&Help"));
    QAction* helpAction = helpMenu->addAction(QStringLiteral("Keyboard and Workflow Help"));
    connect(helpAction, &QAction::triggered, this, &MainWindow::showHelp);
    QAction* optimizationAction = helpMenu->addAction(QStringLiteral("Optimization Summary"));
    connect(optimizationAction, &QAction::triggered, this, &MainWindow::showOptimizationSummary);
}

void MainWindow::createCentralLayout()
{
    auto* splitter = new QSplitter(Qt::Horizontal, this);

    auto* fileModel = new QFileSystemModel(splitter);
    fileModel->setRootPath(QDir::homePath());
    fileModel->setNameFilters(QStringList {
        QStringLiteral("*.aif"),
        QStringLiteral("*.aiff"),
        QStringLiteral("*.flac"),
        QStringLiteral("*.mp3"),
        QStringLiteral("*.ogg"),
        QStringLiteral("*.wav"),
    });
    fileModel->setNameFilterDisables(false);

    auto* fileBrowser = new QTreeView(splitter);
    fileBrowser->setModel(fileModel);
    fileBrowser->setRootIndex(fileModel->index(QDir::homePath()));
    fileBrowser->setDragEnabled(true);
    fileBrowser->setSelectionMode(QAbstractItemView::ExtendedSelection);
    fileBrowser->setHeaderHidden(false);
    fileBrowser->setColumnWidth(0, 220);
    for (int column = 1; column < fileModel->columnCount(); ++column) {
        fileBrowser->hideColumn(column);
    }
    splitter->addWidget(fileBrowser);

    auto* mainPanel = new QWidget(splitter);
    auto* mainLayout = new QVBoxLayout(mainPanel);
    mainLayout->setContentsMargins(10, 10, 10, 10);
    mainLayout->setSpacing(10);

    auto* topControlsScroll = new QScrollArea(mainPanel);
    topControlsScroll->setWidgetResizable(true);
    topControlsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    topControlsScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    topControlsScroll->setFrameShape(QFrame::NoFrame);
    topControlsScroll->setMinimumHeight(96);
    topControlsScroll->setMaximumHeight(148);
    auto* topBar = new QWidget(topControlsScroll);
    auto* topBarLayout = new QGridLayout(topBar);
    topBarLayout->setContentsMargins(0, 0, 0, 0);
    topBarLayout->setHorizontalSpacing(4);
    topBarLayout->setVerticalSpacing(3);

    modeLabel_ = new QLabel(topBar);
    modeLabel_->setAlignment(Qt::AlignCenter);
    modeLabel_->setMinimumWidth(140);
    modeLabel_->setFrameShape(QFrame::StyledPanel);

    octaveLabel_ = new QLabel(topBar);
    octaveLabel_->setAlignment(Qt::AlignCenter);
    octaveLabel_->setMinimumWidth(92);
    octaveLabel_->setFrameShape(QFrame::StyledPanel);
    octaveLabel_->setText(QStringLiteral("Octave +0"));

    recordingMethodComboBox_ = new QComboBox(topBar);
    recordingMethodComboBox_->addItem(QStringLiteral("Key Presses / Automatic"));
    recordingMethodComboBox_->addItem(QStringLiteral("Armed / Middle Pedal"));
    recordingMethodComboBox_->setMinimumWidth(166);

    loopDurationSpinBox_ = new QDoubleSpinBox(topBar);
    loopDurationSpinBox_->setRange(0.10, 60.0);
    loopDurationSpinBox_->setDecimals(2);
    loopDurationSpinBox_->setSingleStep(0.25);
    loopDurationSpinBox_->setSuffix(QStringLiteral(" s"));
    loopDurationSpinBox_->setValue(4.0);

    barsPerRootSpinBox_ = new QSpinBox(topBar);
    barsPerRootSpinBox_->setRange(1, 16);
    barsPerRootSpinBox_->setValue(1);

    tempoLabel_ = new QLabel(QStringLiteral("Tempo -- BPM"), topBar);
    tempoLabel_->setAlignment(Qt::AlignCenter);
    tempoLabel_->setMinimumWidth(106);
    tempoLabel_->setFrameShape(QFrame::StyledPanel);

    rootCheckBox_ = new QCheckBox(QStringLiteral("Root"), topBar);
    syncCheckBox_ = new QCheckBox(QStringLiteral("Sync"), topBar);
    selectedKeyLabel_ = new QLabel(QStringLiteral("Key --"), topBar);
    selectedKeyLabel_->setAlignment(Qt::AlignCenter);
    selectedKeyLabel_->setMinimumWidth(90);
    selectedKeyLabel_->setMaximumWidth(160);
    selectedKeyLabel_->setFrameShape(QFrame::StyledPanel);
    keyAutoCheckBox_ = new QCheckBox(QStringLiteral("Auto"), topBar);
    keyAutoCheckBox_->setEnabled(false);
    selectedKeyVolumeDial_ = new QDial(topBar);
    selectedKeyVolumeDial_->setRange(0, 200);
    selectedKeyVolumeDial_->setValue(100);
    selectedKeyVolumeDial_->setNotchesVisible(true);
    selectedKeyVolumeDial_->setFixedSize(46, 46);
    selectedKeyVolumeDial_->setToolTip(QStringLiteral("Selected key volume"));
    selectedKeyVolumeDial_->setEnabled(false);
    selectedKeyPanDial_ = new QDial(topBar);
    selectedKeyPanDial_->setRange(-100, 100);
    selectedKeyPanDial_->setValue(0);
    selectedKeyPanDial_->setNotchesVisible(true);
    selectedKeyPanDial_->setFixedSize(46, 46);
    selectedKeyPanDial_->setToolTip(QStringLiteral("Selected key pan"));
    selectedKeyPanDial_->setEnabled(false);
    selectedKeyMuteCheckBox_ = new QCheckBox(QStringLiteral("Mute"), topBar);
    selectedKeyMuteCheckBox_->setEnabled(false);
    selectedKeySoloCheckBox_ = new QCheckBox(QStringLiteral("Solo"), topBar);
    selectedKeySoloCheckBox_->setEnabled(false);
    selectedKeySelfMixCheckBox_ = new QCheckBox(QStringLiteral("Self Mix"), topBar);
    selectedKeySelfMixCheckBox_->setEnabled(false);
    selectedKeyGroupComboBox_ = new QComboBox(topBar);
    selectedKeyGroupComboBox_->addItems(QStringList {
        QStringLiteral("Group A"),
        QStringLiteral("Group B"),
        QStringLiteral("Group C"),
        QStringLiteral("Group D"),
    });
    selectedKeyGroupComboBox_->setToolTip(QStringLiteral("Selected key group bus"));
    selectedKeyGroupComboBox_->setFixedWidth(88);
    selectedKeyGroupComboBox_->setEnabled(false);
    selectedKeyPlayPauseButton_ = new QPushButton(QStringLiteral("Play/Pause"), topBar);
    selectedKeyPlayPauseButton_->setMinimumWidth(92);
    selectedKeyPlayPauseButton_->setMaximumWidth(108);
    selectedKeyPlayPauseButton_->setEnabled(false);
    selectedKeySampleLabel_ = new QLabel(QStringLiteral("No key sample"), topBar);
    selectedKeySampleLabel_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    selectedKeySampleLabel_->setMinimumWidth(250);
    selectedKeySampleLabel_->setMaximumWidth(420);
    selectedKeySampleLabel_->setFrameShape(QFrame::StyledPanel);
    masterGainDial_ = new QDial(topBar);
    masterGainDial_->setRange(0, 200);
    masterGainDial_->setValue(100);
    masterGainDial_->setNotchesVisible(true);
    masterGainDial_->setFixedSize(46, 46);
    masterGainDial_->setToolTip(QStringLiteral("Master gain"));
    peakMeter_ = new QProgressBar(topBar);
    peakMeter_->setRange(0, 100);
    peakMeter_->setValue(0);
    peakMeter_->setTextVisible(true);
    peakMeter_->setFormat(QStringLiteral("Peak %p%"));
    peakMeter_->setFixedWidth(95);
    limiterLabel_ = new QLabel(QStringLiteral("Limit"), topBar);
    limiterLabel_->setAlignment(Qt::AlignCenter);
    limiterLabel_->setMinimumWidth(48);
    limiterLabel_->setFrameShape(QFrame::StyledPanel);
    emergencyStopButton_ = new QPushButton(QStringLiteral("Emergency Stop"), topBar);
    emergencyStopButton_->setToolTip(QStringLiteral("Immediately stop all playing samples"));
    emergencyStopButton_->setMinimumWidth(116);
    emergencyStopButton_->setMaximumWidth(168);
    emergencyStopButton_->setStyleSheet(QStringLiteral(
        "QPushButton { background: #b3261e; color: white; font-weight: 700; padding: 6px 10px; }"
        "QPushButton:pressed { background: #7f1d1d; }"));

    topBarLayout->addWidget(modeLabel_, 0, 0, 2, 1);
    topBarLayout->addWidget(octaveLabel_, 2, 0, 1, 1);
    topBarLayout->addWidget(pairWidget(QStringLiteral("Recording"), recordingMethodComboBox_, topBar), 0, 1, 1, 2);
    topBarLayout->addWidget(pairWidget(QStringLiteral("Loop"), loopDurationSpinBox_, topBar), 1, 1, 1, 1);
    topBarLayout->addWidget(pairWidget(QStringLiteral("Bars"), barsPerRootSpinBox_, topBar), 1, 2, 1, 1);
    topBarLayout->addWidget(tempoLabel_, 2, 1, 1, 2);
    topBarLayout->addWidget(rootCheckBox_, 0, 3, 1, 1);
    topBarLayout->addWidget(syncCheckBox_, 0, 4, 1, 1);
    topBarLayout->addWidget(selectedKeyMuteCheckBox_, 1, 3, 1, 1);
    topBarLayout->addWidget(selectedKeySoloCheckBox_, 1, 4, 1, 1);
    topBarLayout->addWidget(keyAutoCheckBox_, 2, 3, 1, 1);
    topBarLayout->addWidget(selectedKeyGroupComboBox_, 2, 4, 1, 1);
    topBarLayout->addWidget(selectedKeyVolumeDial_, 1, 5, 2, 1);
    topBarLayout->addWidget(selectedKeyPanDial_, 1, 6, 2, 1);
    topBarLayout->addWidget(selectedKeyLabel_, 0, 5, 1, 2);
    topBarLayout->addWidget(selectedKeySampleLabel_, 0, 7, 1, 4);
    topBarLayout->addWidget(selectedKeyPlayPauseButton_, 1, 7, 1, 1, Qt::AlignLeft);
    topBarLayout->addWidget(selectedKeySelfMixCheckBox_, 1, 8, 1, 1);
    topBarLayout->addWidget(masterGainDial_, 0, 11, 2, 1);
    topBarLayout->addWidget(peakMeter_, 0, 12, 1, 1);
    topBarLayout->addWidget(limiterLabel_, 1, 12, 1, 1);
    topBarLayout->addWidget(emergencyStopButton_, 2, 12, 1, 1, Qt::AlignRight);
    topBarLayout->setColumnStretch(7, 1);
    topBarLayout->setColumnStretch(8, 1);
    topBarLayout->setColumnStretch(9, 1);
    topBarLayout->setColumnStretch(10, 1);
    topControlsScroll->setWidget(topBar);

    waveformLabel_ = makePanelLabel(QStringLiteral("Waveform View"), mainPanel);
    waveformLabel_->setMinimumHeight(54);
    waveformLabel_->setStyleSheet(QStringLiteral("QLabel { background: #1c1f22; color: #d8dee3; }"));

    samplePoolWidget_ = new SamplePoolWidget(mainPanel);
    samplePoolWidget_->setMinimumHeight(92);

    pianoWidget_ = new PianoWidget(mainPanel);
    auto* pedalBar = new PedalBar(mainPanel);
    auto* bottomStatusWidget = new QWidget(mainPanel);
    auto* bottomStatusLayout = new QHBoxLayout(bottomStatusWidget);
    bottomStatusLayout->setContentsMargins(4, 0, 4, 0);
    bottomStatusLayout->setSpacing(10);
    jackSampleRateLabel_ = new QLabel(QStringLiteral("JACK SR: --"), bottomStatusWidget);
    jackSampleRateLabel_->setFrameShape(QFrame::StyledPanel);
    selectedSampleRateLabel_ = new QLabel(QStringLiteral("Sample SR: --"), bottomStatusWidget);
    selectedSampleRateLabel_->setFrameShape(QFrame::StyledPanel);
    bottomStatusLayout->addWidget(jackSampleRateLabel_);
    bottomStatusLayout->addWidget(selectedSampleRateLabel_, 1);
    dragLineOverlay_ = new DragLineOverlay(mainPanel);

    mainLayout->addWidget(topControlsScroll);
    mainLayout->addWidget(waveformLabel_, 1);
    mainLayout->addWidget(samplePoolWidget_, 2);
    mainLayout->addWidget(pianoWidget_, 0);
    mainLayout->addWidget(pedalBar);
    mainLayout->addWidget(bottomStatusWidget);

    splitter->addWidget(mainPanel);
    fileBrowser->setMinimumWidth(110);
    mainPanel->setMinimumWidth(520);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setCollapsible(0, true);
    splitter->setCollapsible(1, false);
    splitter->setSizes(QList<int> { 210, 900 });

    setCentralWidget(splitter);
    dragLineOverlay_->raise();

    connect(pedalBar, &PedalBar::leftPedalActivated, this, &MainWindow::cycleMode);
    connect(pedalBar, &PedalBar::pedalPressed, this, [this](PedalBar::Pedal pedal) {
        if (pedal == PedalBar::Pedal::Middle) {
            handleMiddlePedalPressed();
        }
    });
    connect(pedalBar, &PedalBar::pedalReleased, this, [this](PedalBar::Pedal pedal) {
        if (pedal == PedalBar::Pedal::Middle) {
            handleMiddlePedalReleased();
        }
    });
    connect(pianoWidget_, &PianoWidget::notePressed, this, &MainWindow::handleNotePressed);
    connect(pianoWidget_, &PianoWidget::noteReleased, this, &MainWindow::handleNoteReleased);
    connect(pianoWidget_, &PianoWidget::octaveChanged, this, &MainWindow::handleOctaveChanged);
    connect(pianoWidget_, &PianoWidget::sampleDropped, this, &MainWindow::handleSampleDroppedOnKey);
    connect(pianoWidget_, &PianoWidget::keyContextMenuRequested, this, &MainWindow::showKeyContextMenu);
    connect(pianoWidget_, &PianoWidget::sampleDragLineChanged, this, [this](
        const QPoint& globalStart,
        const QPoint& globalEnd,
        bool active) {
        if (dragLineOverlay_ != nullptr) {
            dragLineOverlay_->setLine(globalStart, globalEnd, active);
        }
    });
    connect(samplePoolWidget_, &SamplePoolWidget::sampleSelected, this, &MainWindow::handleSampleSelected);
    connect(samplePoolWidget_, &SamplePoolWidget::sampleRemoved, this, &MainWindow::handleSampleRemoved);
    connect(samplePoolWidget_, &SamplePoolWidget::statusMessage, this, &MainWindow::showStatusMessage);
    connect(samplePoolWidget_, &SamplePoolWidget::layeredPlayRequested, this, &MainWindow::handleLayeredPlayRequested);
    connect(samplePoolWidget_, &SamplePoolWidget::appendedSampleCreated, this, &MainWindow::handleSampleSelected);
    connect(samplePoolWidget_, &SamplePoolWidget::markerPreviewStarted, this, [this](
        Sample* sample,
        size_t startFrame,
        int direction,
        double speedRatio) {
        audioEngine_.startPreview(sample, startFrame, direction, speedRatio);
    });
    connect(samplePoolWidget_, &SamplePoolWidget::markerPreviewStopped, this, [this] {
        audioEngine_.stopPreview();
    });
    connect(rootCheckBox_, &QCheckBox::toggled, this, &MainWindow::handleRootToggled);
    connect(syncCheckBox_, &QCheckBox::toggled, this, &MainWindow::handleSyncToggled);
    connect(keyAutoCheckBox_, &QCheckBox::toggled, this, &MainWindow::handleKeyAutoToggled);
    connect(selectedKeyPlayPauseButton_, &QPushButton::clicked, this, &MainWindow::handleSelectedKeyPlayPause);
    connect(selectedKeyVolumeDial_, &QDial::valueChanged, this, &MainWindow::handleSelectedKeyVolumeChanged);
    connect(selectedKeyPanDial_, &QDial::valueChanged, this, &MainWindow::handleSelectedKeyPanChanged);
    connect(selectedKeyMuteCheckBox_, &QCheckBox::toggled, this, &MainWindow::handleSelectedKeyMuteToggled);
    connect(selectedKeySoloCheckBox_, &QCheckBox::toggled, this, &MainWindow::handleSelectedKeySoloToggled);
    connect(selectedKeySelfMixCheckBox_, &QCheckBox::toggled, this, &MainWindow::handleSelectedKeySelfMixToggled);
    connect(selectedKeyGroupComboBox_, qOverload<int>(&QComboBox::currentIndexChanged), this, &MainWindow::handleSelectedKeyGroupChanged);
    connect(masterGainDial_, &QDial::valueChanged, this, &MainWindow::handleMasterGainChanged);
    connect(emergencyStopButton_, &QPushButton::clicked, this, &MainWindow::handleEmergencyStop);
    connect(recordingMethodComboBox_, qOverload<int>(&QComboBox::currentIndexChanged), this, &MainWindow::setRecordingMethod);
    connect(barsPerRootSpinBox_, qOverload<int>(&QSpinBox::valueChanged), this, &MainWindow::handleBarsPerRootChanged);
    connect(&midiInput_, &MidiInput::midiNoteOn, this, &MainWindow::handleMidiNoteOn);
    connect(&midiInput_, &MidiInput::midiNoteOff, this, &MainWindow::handleMidiNoteOff);
    connect(&midiInput_, &MidiInput::leftPedalActivated, this, &MainWindow::cycleMode);
    connect(&midiInput_, &MidiInput::middlePedalPressed, this, &MainWindow::handleMiddlePedalPressed);
    connect(&midiInput_, &MidiInput::middlePedalReleased, this, &MainWindow::handleMiddlePedalReleased);

    auto* leftPedalShortcut = new QShortcut(QKeySequence(Qt::Key_QuoteLeft), this);
    connect(leftPedalShortcut, &QShortcut::activated, this, &MainWindow::cycleMode);
}

void MainWindow::cycleMode()
{
    if (mode_ == AppMode::Record && activeMiddlePedalRecordingNoteIndex_ >= 0) {
        handleMiddlePedalReleased();
    }

    switch (mode_) {
    case AppMode::Normal:
        mode_ = AppMode::Record;
        break;
    case AppMode::Record:
        mode_ = AppMode::Playback;
        break;
    case AppMode::Playback:
        mode_ = AppMode::Edit;
        break;
    case AppMode::Edit:
        mode_ = AppMode::Normal;
        break;
    }

    updateModeLabel();
    updateKeySettingsControls(selectedNoteIndex_);
    if (mode_ == AppMode::Edit && selectedNoteIndex_ >= 0) {
        pianoWidget_->setSelectedNoteIndex(selectedNoteIndex_);
    } else if (mode_ != AppMode::Edit) {
        pianoWidget_->setSelectedNoteIndex(-1);
    }
    statusBar()->showMessage(QStringLiteral("Mode: %1").arg(modeName(mode_)));
}

void MainWindow::handleNotePressed(int noteIndex)
{
    Sample* assignedSample = assignedSamples_[static_cast<size_t>(noteIndex)];

    switch (mode_) {
    case AppMode::Record:
        selectedNoteIndex_ = noteIndex;
        updateKeySettingsControls(noteIndex);
        if (recordingMethod_ == RecordingMethod::ArmedMiddlePedal) {
            if (activeMiddlePedalRecordingNoteIndex_ >= 0) {
                statusBar()->showMessage(QStringLiteral("Recording %1; release the middle pedal before arming another key.")
                    .arg(noteNameForIndex(activeMiddlePedalRecordingNoteIndex_)));
                return;
            }

            armedRecordingNoteIndex_ = noteIndex;
            statusBar()->showMessage(QStringLiteral("Armed %1. Press the middle pedal to record.")
                .arg(noteNameForIndex(noteIndex)));
            return;
        }

        startRecordingForNote(noteIndex);
        return;
    case AppMode::Playback:
        if (assignedSample != nullptr) {
            if (perKeyLoopMode_[static_cast<size_t>(noteIndex)] == KeyLoopMode::NoLoop) {
                audioEngine_.noteOn(noteIndex);
                statusBar()->showMessage(
                    QStringLiteral("Triggered %1 on %2").arg(assignedSample->name, noteNameForIndex(noteIndex)));
            } else {
                audioEngine_.togglePlayback(noteIndex);
                statusBar()->showMessage(
                    QStringLiteral("Toggled %1 on %2").arg(assignedSample->name, noteNameForIndex(noteIndex)));
            }
            return;
        }
        break;
    case AppMode::Normal:
        break;
    case AppMode::Edit:
        selectKeyForEdit(noteIndex);
        return;
    }

    if (assignedSample != nullptr) {
        statusBar()->showMessage(
            QStringLiteral("%1 assigned to %2").arg(assignedSample->name, noteNameForIndex(noteIndex)));
        return;
    }

    statusBar()->showMessage(
        QStringLiteral("%1 pressed in %2 mode").arg(noteNameForIndex(noteIndex), modeName(mode_)));
}

void MainWindow::handleNoteReleased(int noteIndex)
{
    if (mode_ == AppMode::Edit) {
        return;
    }

    if (mode_ == AppMode::Record) {
        if (recordingMethod_ == RecordingMethod::ArmedMiddlePedal) {
            if (armedRecordingNoteIndex_ == noteIndex && activeMiddlePedalRecordingNoteIndex_ < 0) {
                statusBar()->showMessage(QStringLiteral("%1 armed. Middle pedal controls recording.")
                    .arg(noteNameForIndex(noteIndex)));
            }
            return;
        }

        finishRecordingForNote(noteIndex);
        return;
    }

    if (mode_ == AppMode::Playback
        && assignedSamples_[static_cast<size_t>(noteIndex)] != nullptr
        && perKeyLoopMode_[static_cast<size_t>(noteIndex)] == KeyLoopMode::NoLoop) {
        audioEngine_.noteOff(noteIndex);
        statusBar()->showMessage(
            QStringLiteral("%1 released on %2").arg(assignedSamples_[static_cast<size_t>(noteIndex)]->name, noteNameForIndex(noteIndex)));
        return;
    }

    statusBar()->showMessage(
        QStringLiteral("%1 released in %2 mode").arg(noteNameForIndex(noteIndex), modeName(mode_)));
}

void MainWindow::handleOctaveChanged(int octaveOffset)
{
    octaveLabel_->setText(QStringLiteral("Octave %1%2")
        .arg(octaveOffset >= 0 ? QStringLiteral("+") : QString())
        .arg(octaveOffset));
}

void MainWindow::handleSampleSelected(Sample* sample)
{
    updateWaveformView(sample);
    updateSampleFlagControls(sample);
    updateSelectedKeyControls(selectedNoteIndex_);
    updateBottomStatus();
    if (sample == nullptr) {
        return;
    }

    statusBar()->showMessage(QStringLiteral("Selected %1").arg(sample->name));
}

void MainWindow::handleSampleRemoved(Sample* sample)
{
    if (sample == nullptr) {
        return;
    }

    audioEngine_.detachSample(sample);
    stretchRequestIds_.erase(sample);

    for (Sample*& assignedSample : assignedSamples_) {
        if (assignedSample == sample) {
            assignedSample = nullptr;
        }
    }

    if (rootSample_ == sample) {
        rootSample_ = nullptr;
        globalLoopDurationSec_ = 0.0;
        tempoBpm_ = 0.0;
        tempoLabel_->setText(QStringLiteral("Tempo -- BPM"));
    }

    updateSampleFlagControls(nullptr);
    updateSelectedKeyControls(selectedNoteIndex_);
    updateBottomStatus();
}

void MainWindow::handleSampleDroppedOnKey(int noteIndex, Sample* sample)
{
    if (noteIndex < 0 || noteIndex >= PianoWidget::kKeyCount || sample == nullptr) {
        return;
    }

    selectedNoteIndex_ = noteIndex;
    if (mode_ == AppMode::Edit) {
        pianoWidget_->setSelectedNoteIndex(noteIndex);
    }
    updateKeySettingsControls(noteIndex);
    assignSampleToKey(noteIndex, sample);
    statusBar()->showMessage(QStringLiteral("Assigned %1 to %2").arg(sample->name, noteNameForIndex(noteIndex)));
}

void MainWindow::handleLayeredPlayRequested(Sample* first, Sample* second)
{
    if (first == nullptr || second == nullptr) {
        return;
    }

    audioEngine_.playLayered(first, second);
    statusBar()->showMessage(QStringLiteral("Layered play queued for %1 + %2").arg(first->name, second->name));
}

void MainWindow::showStatusMessage(const QString& message)
{
    statusBar()->showMessage(message);
}

void MainWindow::pollFinishedRecordings()
{
    updatePeakMeter();

    FinishedRecording recording;
    while (audioEngine_.takeFinishedRecording(&recording)) {
        if (recording.frames == 0U || recording.noteIndex < 0 || recording.noteIndex >= PianoWidget::kKeyCount) {
            statusBar()->showMessage(QStringLiteral("Recording was empty."));
            continue;
        }

        const QString sampleName = QStringLiteral("%1 recording").arg(noteNameForIndex(recording.noteIndex));
        Sample* sample = samplePoolWidget_->addRecordedSample(
            sampleName,
            std::move(recording.data),
            recording.sampleRate,
            recording.frames);
        if (sample == nullptr) {
            continue;
        }

        assignSampleToKey(recording.noteIndex, sample);
        if (rootSample_ == nullptr) {
            setRootSample(sample);
        }
        statusBar()->showMessage(QStringLiteral("Recorded and assigned %1").arg(sample->name));
        recording = FinishedRecording {};
    }
}

void MainWindow::handleRootToggled(bool enabled)
{
    if (updatingSampleFlagControls_) {
        return;
    }

    Sample* selected = samplePoolWidget_->selectedSample();
    if (selected == nullptr) {
        return;
    }

    if (enabled) {
        setRootSample(selected);
        return;
    }

    clearRootSample(selected);
}

void MainWindow::handleSyncToggled(bool enabled)
{
    if (updatingSampleFlagControls_) {
        return;
    }

    Sample* selected = samplePoolWidget_->selectedSample();
    if (selected == nullptr) {
        return;
    }

    if (enabled && selected == rootSample_) {
        selected->syncToRoot = false;
        updateSampleFlagControls(selected);
        statusBar()->showMessage(QStringLiteral("Root sample does not need Sync."));
        return;
    }

    selected->syncToRoot = enabled;
    if (!enabled) {
        selected->timingSyncState = TimingSyncState::Idle;
        selected->timingError.clear();
        selected->syncedFromTempoBpm = 0.0;
        if (selected->timingAnalysisState == TimingAnalysisState::Ready) {
            selected->currentTempoBpm = selected->analyzedTempoBpm;
        } else {
            selected->currentTempoBpm = 0.0;
        }
    }
    updateSampleFlagControls(selected);
    updateBottomStatus();

    if (enabled) {
        selected->timingAnalysisState = TimingAnalysisState::Unanalyzed;
        selected->timingSyncState = TimingSyncState::PendingAnalysis;
        selected->timingError.clear();
        selected->hasTimingAnalysis = false;
        selected->hasBeatGrid = false;
        selected->detectedTempoBpm = 0.0;
        selected->syncedFromTempoBpm = 0.0;
        selected->currentTempoBpm = 0.0;
        selected->analyzedRootStartFrame = 0.0;
        selected->analyzedActiveEndFrame = 0.0;
        selected->analyzedBeatPeriodFrames = 0.0;
        selected->analyzedTempoBpm = 0.0;
        selected->analyzedTempoConfidence = 0.0;
        selected->analyzedBeatCount = 0;
        selected->analyzedOnsetCount = 0;
        updateBottomStatus();
        updateSampleTimingStateForSync(selected);
        return;
    }

    statusBar()->showMessage(QStringLiteral("Sync disabled for %1").arg(selected->name));
}

void MainWindow::handleBarsPerRootChanged(int bars)
{
    if (rootSample_ != nullptr && rootSample_->timingAnalysisState == TimingAnalysisState::Processing) {
        tempoLabel_->setText(QStringLiteral("BPM: Processing"));
        updateBottomStatus();
        return;
    }

    if (rootSample_ != nullptr && rootSample_->hasTimingAnalysis && rootSample_->hasBeatGrid && rootSample_->sampleRate > 0U) {
        const double previousStart = rootSample_->startFrame;
        const double previousEnd = rootSample_->endFrame;
        const double startFrame = std::clamp(rootSample_->analyzedRootStartFrame, 0.0, static_cast<double>(rootSample_->frames));
        const double desiredEndFrame = std::clamp(
            startFrame + (rootSample_->analyzedBeatPeriodFrames * static_cast<double>(std::max(1, bars) * 4)),
            startFrame,
            static_cast<double>(rootSample_->frames));
        const double analyzedStart = startFrame / static_cast<double>(rootSample_->frames);
        const double analyzedEnd = desiredEndFrame / static_cast<double>(rootSample_->frames);
        rootSample_->startFrame = analyzedStart;
        rootSample_->endFrame = analyzedEnd;
        rootSample_->analyzedActiveEndFrame = desiredEndFrame;
        applyRootTimingTrimToAssignedKeys(rootSample_, previousStart, previousEnd, analyzedStart, analyzedEnd);
        if (samplePoolWidget_->selectedSample() == rootSample_) {
            updateWaveformView(rootSample_);
        }
    }

    updateTempoFromRoot();
    syncAllSamplesToRoot();
    updateBottomStatus();
}

void MainWindow::handleStretchFinished(const StretchResult& result)
{
    auto requestIt = stretchRequestIds_.find(result.sample);
    if (requestIt == stretchRequestIds_.end() || requestIt->second != result.requestId) {
        return;
    }
    stretchRequestIds_.erase(requestIt);

    if (!result.ok) {
        if (result.sample != nullptr) {
            result.sample->timingSyncState = TimingSyncState::Failed;
            result.sample->timingError = result.errorMessage;
            result.sample->syncedFromTempoBpm = 0.0;
            if (result.sample->timingAnalysisState == TimingAnalysisState::Ready) {
                result.sample->currentTempoBpm = result.sample->analyzedTempoBpm;
            } else {
                result.sample->currentTempoBpm = 0.0;
            }
        }
        updateBottomStatus();
        statusBar()->showMessage(QStringLiteral("Stretch failed for %1: %2").arg(result.sampleName, result.errorMessage));
        return;
    }

    audioEngine_.detachSample(result.sample);
    samplePoolWidget_->replaceSampleAudio(result.sample, result.stereoData, result.sampleRate, result.frames);
    result.sample->startFrame = 0.0;
    result.sample->endFrame = 1.0;
    result.sample->timingSyncState = TimingSyncState::Synced;
    result.sample->timingError.clear();
    result.sample->syncedFromTempoBpm = result.sample->detectedTempoBpm;
    result.sample->currentTempoBpm = tempoBpm_;
    result.sample->timingAnalysisState = TimingAnalysisState::Unanalyzed;
    result.sample->hasTimingAnalysis = false;
    result.sample->hasBeatGrid = false;
    result.sample->analyzedTempoBpm = 0.0;
    result.sample->analyzedBeatPeriodFrames = 0.0;
    result.sample->analyzedRootStartFrame = 0.0;
    result.sample->analyzedActiveEndFrame = 0.0;

    for (int noteIndex = 0; noteIndex < PianoWidget::kKeyCount; ++noteIndex) {
        if (assignedSamples_[static_cast<size_t>(noteIndex)] == result.sample) {
            perKeyTrimStart_[static_cast<size_t>(noteIndex)] = 0.0;
            perKeyTrimEnd_[static_cast<size_t>(noteIndex)] = 1.0;
            audioEngine_.assignSampleToKey(noteIndex, result.sample);
            audioEngine_.setKeyTrimRange(noteIndex, 0.0, 1.0);
        }
    }

    updateWaveformView(samplePoolWidget_->selectedSample());
    if (selectedNoteIndex_ >= 0 && selectedNoteIndex_ < PianoWidget::kKeyCount) {
        updateKeySettingsControls(selectedNoteIndex_);
    }
    updateBottomStatus();
    statusBar()->showMessage(QStringLiteral("Stretched %1 to %2 s")
        .arg(result.sampleName)
        .arg(result.targetDurationSec, 0, 'f', 2));
}

void MainWindow::handleRootTimingFinished(const RootTimingResult& result)
{
    auto requestIt = rootTimingRequestIds_.find(result.sample);
    if (requestIt == rootTimingRequestIds_.end() || requestIt->second != result.requestId) {
        return;
    }
    rootTimingRequestIds_.erase(requestIt);

    if (!result.ok || result.sample == nullptr) {
        if (result.sample != nullptr) {
            result.sample->timingAnalysisState = TimingAnalysisState::Failed;
            result.sample->timingError = result.errorMessage;
            result.sample->detectedTempoBpm = 0.0;
            result.sample->currentTempoBpm = 0.0;
            if (result.sample == rootSample_) {
                globalLoopDurationSec_ = 0.0;
                tempoBpm_ = 0.0;
                tempoLabel_->setText(QStringLiteral("BPM: Analysis failed"));
            }
            if (result.sample->syncToRoot && result.sample != rootSample_) {
                result.sample->timingSyncState = TimingSyncState::Failed;
            }
        }
        updateBottomStatus();
        statusBar()->showMessage(QStringLiteral("Root timing analysis failed for %1: %2")
            .arg(result.sampleName, result.errorMessage));
        return;
    }

    const double previousStart = result.sample->startFrame;
    const double previousEnd = result.sample->endFrame;
    const double analyzedStart = std::clamp(
        result.suggestedStartFrame / static_cast<double>(std::max<size_t>(1U, result.sample->frames)),
        0.0,
        1.0);
    const double analyzedEnd = std::clamp(
        result.suggestedEndFrame / static_cast<double>(std::max<size_t>(1U, result.sample->frames)),
        analyzedStart,
        1.0);

    result.sample->hasTimingAnalysis = true;
    result.sample->hasBeatGrid = result.hasBeatGrid;
    result.sample->timingAnalysisState = result.hasBeatGrid
        ? TimingAnalysisState::Ready
        : TimingAnalysisState::Failed;
    result.sample->timingError = result.hasBeatGrid
        ? QString()
        : QStringLiteral("No stable beat grid found");
    result.sample->analyzedRootStartFrame = result.suggestedStartFrame;
    result.sample->analyzedActiveEndFrame = result.suggestedEndFrame;
    result.sample->analyzedBeatPeriodFrames = result.beatPeriodFrames;
    result.sample->analyzedTempoBpm = result.tempoBpm;
    result.sample->detectedTempoBpm = result.tempoBpm;
    result.sample->currentTempoBpm = result.hasBeatGrid ? result.tempoBpm : 0.0;
    result.sample->analyzedTempoConfidence = result.confidence;
    result.sample->analyzedBeatCount = result.beatCount;
    result.sample->analyzedOnsetCount = result.onsetCount;
    result.sample->startFrame = analyzedStart;
    result.sample->endFrame = analyzedEnd;
    applyRootTimingTrimToAssignedKeys(result.sample, previousStart, previousEnd, analyzedStart, analyzedEnd);

    if (result.sample == rootSample_) {
        updateTempoFromRoot();
        syncAllSamplesToRoot();
    }
    if (result.sample->syncToRoot && result.sample != rootSample_) {
        if (rootSample_ != nullptr && tempoBpm_ > 0.0 && result.sample->detectedTempoBpm > 0.0) {
            statusBar()->showMessage(QStringLiteral("Detected %1 BPM for %2. Syncing to %3 BPM")
                .arg(result.sample->detectedTempoBpm, 0, 'f', 1)
                .arg(result.sample->name)
                .arg(tempoBpm_, 0, 'f', 1));
        }
        updateBottomStatus();
        updateSampleTimingStateForSync(result.sample);
    }

    if (samplePoolWidget_->selectedSample() == result.sample) {
        updateWaveformView(result.sample);
        updateSampleFlagControls(result.sample);
    }
    updateBottomStatus();
    statusBar()->showMessage(result.message);
}

void MainWindow::handleKeyAutoToggled(bool enabled)
{
    if (updatingKeySettingsControls_ || selectedNoteIndex_ < 0 || selectedNoteIndex_ >= PianoWidget::kKeyCount) {
        return;
    }

    perKeyAutoMode_[static_cast<size_t>(selectedNoteIndex_)] = enabled;
    statusBar()->showMessage(QStringLiteral("%1 recording mode: %2")
        .arg(noteNameForIndex(selectedNoteIndex_), enabled ? QStringLiteral("Auto") : QStringLiteral("Hard")));
}

void MainWindow::handleMidiNoteOn(int midiNote)
{
    const int noteIndex = midiNote - PianoWidget::kLowestMidiNote;
    if (noteIndex < 0 || noteIndex >= PianoWidget::kKeyCount) {
        return;
    }

    handleNotePressed(noteIndex);
}

void MainWindow::handleMidiNoteOff(int midiNote)
{
    const int noteIndex = midiNote - PianoWidget::kLowestMidiNote;
    if (noteIndex < 0 || noteIndex >= PianoWidget::kKeyCount) {
        return;
    }

    handleNoteReleased(noteIndex);
}

void MainWindow::handleEmergencyStop()
{
    audioEngine_.stopAllPlayback();
    statusBar()->showMessage(QStringLiteral("Emergency stop: all sample playback stopped."));
}

void MainWindow::selectKeyForEdit(int noteIndex)
{
    if (noteIndex < 0 || noteIndex >= PianoWidget::kKeyCount) {
        return;
    }

    selectedNoteIndex_ = noteIndex;
    pianoWidget_->setSelectedNoteIndex(noteIndex);
    updateKeySettingsControls(noteIndex);
    Sample* sample = assignedSamples_[static_cast<size_t>(noteIndex)];
    statusBar()->showMessage(sample == nullptr
            ? QStringLiteral("Selected %1 for editing").arg(noteNameForIndex(noteIndex))
            : QStringLiteral("Selected %1 for editing: %2").arg(noteNameForIndex(noteIndex), sample->name));
}

void MainWindow::handleSelectedKeyPlayPause()
{
    if (mode_ != AppMode::Edit) {
        return;
    }

    if (selectedNoteIndex_ < 0 || selectedNoteIndex_ >= PianoWidget::kKeyCount) {
        return;
    }

    Sample* sample = assignedSamples_[static_cast<size_t>(selectedNoteIndex_)];
    if (sample == nullptr) {
        statusBar()->showMessage(QStringLiteral("No sample assigned to %1").arg(noteNameForIndex(selectedNoteIndex_)));
        return;
    }

    audioEngine_.togglePausePlayback(selectedNoteIndex_);
    statusBar()->showMessage(QStringLiteral("Play/Pause %1 on %2").arg(sample->name, noteNameForIndex(selectedNoteIndex_)));
}

void MainWindow::handleSelectedKeyVolumeChanged(int value)
{
    if (mode_ != AppMode::Edit
        || updatingKeySettingsControls_
        || selectedNoteIndex_ < 0
        || selectedNoteIndex_ >= PianoWidget::kKeyCount) {
        return;
    }

    perKeyVolumePercent_[static_cast<size_t>(selectedNoteIndex_)] = value;
    audioEngine_.setKeyVolume(selectedNoteIndex_, static_cast<float>(value) / 100.0F);
    statusBar()->showMessage(QStringLiteral("%1 volume: %2%").arg(noteNameForIndex(selectedNoteIndex_)).arg(value));
}

void MainWindow::handleSelectedKeyPanChanged(int value)
{
    if (mode_ != AppMode::Edit
        || updatingKeySettingsControls_
        || selectedNoteIndex_ < 0
        || selectedNoteIndex_ >= PianoWidget::kKeyCount) {
        return;
    }

    perKeyPanPercent_[static_cast<size_t>(selectedNoteIndex_)] = value;
    audioEngine_.setKeyPan(selectedNoteIndex_, static_cast<float>(value) / 100.0F);
    statusBar()->showMessage(QStringLiteral("%1 pan: %2").arg(noteNameForIndex(selectedNoteIndex_)).arg(value));
}

void MainWindow::handleSelectedKeyMuteToggled(bool enabled)
{
    if (mode_ != AppMode::Edit || updatingKeySettingsControls_ || selectedNoteIndex_ < 0 || selectedNoteIndex_ >= PianoWidget::kKeyCount) {
        return;
    }

    perKeyMuted_[static_cast<size_t>(selectedNoteIndex_)] = enabled;
    audioEngine_.setKeyMuted(selectedNoteIndex_, enabled);
}

void MainWindow::handleSelectedKeySoloToggled(bool enabled)
{
    if (mode_ != AppMode::Edit || updatingKeySettingsControls_ || selectedNoteIndex_ < 0 || selectedNoteIndex_ >= PianoWidget::kKeyCount) {
        return;
    }

    perKeySolo_[static_cast<size_t>(selectedNoteIndex_)] = enabled;
    audioEngine_.setKeySolo(selectedNoteIndex_, enabled);
}

void MainWindow::handleSelectedKeySelfMixToggled(bool enabled)
{
    if (mode_ != AppMode::Edit || updatingKeySettingsControls_ || selectedNoteIndex_ < 0 || selectedNoteIndex_ >= PianoWidget::kKeyCount) {
        return;
    }

    perKeySelfMixEnabled_[static_cast<size_t>(selectedNoteIndex_)] = enabled;
    audioEngine_.setKeySelfMixEnabled(selectedNoteIndex_, enabled);
    statusBar()->showMessage(QStringLiteral("%1 self-mix: %2")
        .arg(noteNameForIndex(selectedNoteIndex_), enabled ? QStringLiteral("On") : QStringLiteral("Off")));
}

void MainWindow::handleSelectedKeyGroupChanged(int index)
{
    if (mode_ != AppMode::Edit || updatingKeySettingsControls_ || selectedNoteIndex_ < 0 || selectedNoteIndex_ >= PianoWidget::kKeyCount) {
        return;
    }

    perKeyGroup_[static_cast<size_t>(selectedNoteIndex_)] = std::clamp(index, 0, 3);
    audioEngine_.setKeyGroup(selectedNoteIndex_, perKeyGroup_[static_cast<size_t>(selectedNoteIndex_)]);
}

void MainWindow::handleMasterGainChanged(int value)
{
    masterGainPercent_ = std::clamp(value, 0, 200);
    audioEngine_.setMasterGain(static_cast<float>(masterGainPercent_) / 100.0F);
    statusBar()->showMessage(QStringLiteral("Master gain: %1%").arg(masterGainPercent_));
}

void MainWindow::showSettingsDialog()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("SuperLooper Audio and Mixer Settings"));
    dialog.resize(std::min(width() - 80, 720), std::min(height() - 80, 620));
    auto* outerLayout = new QVBoxLayout(&dialog);
    auto* scrollArea = new QScrollArea(&dialog);
    scrollArea->setWidgetResizable(true);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    auto* content = new QWidget(scrollArea);
    auto* layout = new QFormLayout(content);
    scrollArea->setWidget(content);
    outerLayout->addWidget(scrollArea);

    auto* resamplerCombo = new QComboBox(&dialog);
    const QStringList backends = SampleLoader::availableResamplerBackendNames();
    resamplerCombo->addItems(backends);
    const int backendIndex = backends.indexOf(SampleLoader::resamplerBackendName(resamplerBackend_));
    resamplerCombo->setCurrentIndex(backendIndex >= 0 ? backendIndex : 0);

    auto* qualityCombo = new QComboBox(&dialog);
    const auto refreshQualityCombo = [resamplerCombo, qualityCombo, this]() {
        const ResamplerBackend selectedBackend = SampleLoader::resamplerBackendFromName(resamplerCombo->currentText());
        const QStringList qualityNames = SampleLoader::availableResamplerQualityNames(selectedBackend);
        const QString currentName = SampleLoader::resamplerQualityName(resamplerQuality_);
        QSignalBlocker blocker(qualityCombo);
        qualityCombo->clear();
        qualityCombo->addItems(qualityNames);
        int qualityIndex = qualityNames.indexOf(currentName);
        if (qualityIndex < 0) {
            qualityIndex = qualityNames.indexOf(SampleLoader::resamplerQualityName(
                selectedBackend == ResamplerBackend::Automatic
                    ? ResamplerQuality::Automatic
                    : SampleLoader::bestQualityForBackend(selectedBackend)));
        }
        qualityCombo->setCurrentIndex(qualityIndex >= 0 ? qualityIndex : 0);
    };
    refreshQualityCombo();
    connect(resamplerCombo, &QComboBox::currentTextChanged, &dialog, refreshQualityCombo);

    auto* normalizationCheckBox = new QCheckBox(QStringLiteral("Apply analyzed sample normalization"), &dialog);
    normalizationCheckBox->setChecked(sampleNormalizationEnabled_);

    auto* limiterCheckBox = new QCheckBox(QStringLiteral("Enable final soft limiter"), &dialog);
    limiterCheckBox->setChecked(limiterEnabled_);

    auto* fadeSpinBox = new QDoubleSpinBox(&dialog);
    fadeSpinBox->setRange(0.0, 60.0);
    fadeSpinBox->setDecimals(1);
    fadeSpinBox->setSuffix(QStringLiteral(" ms"));
    fadeSpinBox->setValue(fadeTimeMs_);

    auto* crossfadeCheckBox = new QCheckBox(QStringLiteral("Enable loop-boundary crossfade"), &dialog);
    crossfadeCheckBox->setChecked(loopCrossfadeEnabled_);

    auto* crossfadeSpinBox = new QDoubleSpinBox(&dialog);
    crossfadeSpinBox->setRange(1.0, 60.0);
    crossfadeSpinBox->setDecimals(1);
    crossfadeSpinBox->setSuffix(QStringLiteral(" ms"));
    crossfadeSpinBox->setValue(loopCrossfadeMs_);

    auto* masterGainSpinBox = new QSpinBox(&dialog);
    masterGainSpinBox->setRange(0, 200);
    masterGainSpinBox->setSuffix(QStringLiteral(" %"));
    masterGainSpinBox->setValue(masterGainPercent_);

    auto* beatScopeCombo = new QComboBox(&dialog);
    beatScopeCombo->addItems(QStringList {
        beatDetectionScopeName(BeatDetectionScope::Beginning),
        beatDetectionScopeName(BeatDetectionScope::End),
        beatDetectionScopeName(BeatDetectionScope::BeginningAndEndAverage),
        beatDetectionScopeName(BeatDetectionScope::EntireSample),
    });
    beatScopeCombo->setCurrentText(beatDetectionScopeName(beatDetectionScope_));

    auto* beatStartLengthSpinBox = new QDoubleSpinBox(&dialog);
    beatStartLengthSpinBox->setRange(5.0, 120.0);
    beatStartLengthSpinBox->setDecimals(0);
    beatStartLengthSpinBox->setSuffix(QStringLiteral(" s"));
    beatStartLengthSpinBox->setValue(beatDetectionStartLengthSeconds_);

    auto* beatEndLengthSpinBox = new QDoubleSpinBox(&dialog);
    beatEndLengthSpinBox->setRange(5.0, 120.0);
    beatEndLengthSpinBox->setDecimals(0);
    beatEndLengthSpinBox->setSuffix(QStringLiteral(" s"));
    beatEndLengthSpinBox->setValue(beatDetectionEndLengthSeconds_);

    auto* beatMergeCombo = new QComboBox(&dialog);
    beatMergeCombo->addItems(QStringList {
        beatDetectionMergePolicyName(BeatDetectionMergePolicy::PreferHigherConfidence),
        beatDetectionMergePolicyName(BeatDetectionMergePolicy::Average),
    });
    beatMergeCombo->setCurrentText(beatDetectionMergePolicyName(beatDetectionMergePolicy_));
    const auto refreshBeatDetectionControls = [beatScopeCombo, beatStartLengthSpinBox, beatEndLengthSpinBox, beatMergeCombo]() {
        const BeatDetectionScope scope = beatDetectionScopeFromName(beatScopeCombo->currentText());
        const bool useStart = scope == BeatDetectionScope::Beginning || scope == BeatDetectionScope::BeginningAndEndAverage;
        const bool useEnd = scope == BeatDetectionScope::End || scope == BeatDetectionScope::BeginningAndEndAverage;
        beatStartLengthSpinBox->setEnabled(useStart);
        beatEndLengthSpinBox->setEnabled(useEnd);
        beatMergeCombo->setEnabled(scope == BeatDetectionScope::BeginningAndEndAverage);
    };
    refreshBeatDetectionControls();
    connect(beatScopeCombo, &QComboBox::currentTextChanged, &dialog, refreshBeatDetectionControls);

    layout->addRow(QStringLiteral("Resampler"), resamplerCombo);
    layout->addRow(QStringLiteral("Resampler quality"), qualityCombo);
    layout->addRow(QStringLiteral("Beat detection scope"), beatScopeCombo);
    layout->addRow(QStringLiteral("Beat detection start length"), beatStartLengthSpinBox);
    layout->addRow(QStringLiteral("Beat detection end length"), beatEndLengthSpinBox);
    layout->addRow(QStringLiteral("First+Last merge"), beatMergeCombo);
    layout->addRow(QStringLiteral("Per-sample gain"), normalizationCheckBox);
    layout->addRow(QStringLiteral("Limiter"), limiterCheckBox);
    layout->addRow(QStringLiteral("Fade time"), fadeSpinBox);
    layout->addRow(QStringLiteral("Loop crossfade"), crossfadeCheckBox);
    layout->addRow(QStringLiteral("Crossfade time"), crossfadeSpinBox);
    layout->addRow(QStringLiteral("Master gain"), masterGainSpinBox);

    std::array<QSpinBox*, 4> groupGainSpinBoxes {};
    std::array<QSpinBox*, 4> groupPanSpinBoxes {};
    std::array<QCheckBox*, 4> groupMuteCheckBoxes {};
    std::array<QCheckBox*, 4> groupSoloCheckBoxes {};
    for (int group = 0; group < 4; ++group) {
        groupGainSpinBoxes[static_cast<size_t>(group)] = new QSpinBox(&dialog);
        groupGainSpinBoxes[static_cast<size_t>(group)]->setRange(0, 200);
        groupGainSpinBoxes[static_cast<size_t>(group)]->setSuffix(QStringLiteral(" %"));
        groupGainSpinBoxes[static_cast<size_t>(group)]->setValue(groupGainPercent_[static_cast<size_t>(group)]);
        groupPanSpinBoxes[static_cast<size_t>(group)] = new QSpinBox(&dialog);
        groupPanSpinBoxes[static_cast<size_t>(group)]->setRange(-100, 100);
        groupPanSpinBoxes[static_cast<size_t>(group)]->setValue(groupPanPercent_[static_cast<size_t>(group)]);
        groupMuteCheckBoxes[static_cast<size_t>(group)] = new QCheckBox(QStringLiteral("Mute"), &dialog);
        groupMuteCheckBoxes[static_cast<size_t>(group)]->setChecked(groupMuted_[static_cast<size_t>(group)]);
        groupSoloCheckBoxes[static_cast<size_t>(group)] = new QCheckBox(QStringLiteral("Solo"), &dialog);
        groupSoloCheckBoxes[static_cast<size_t>(group)]->setChecked(groupSolo_[static_cast<size_t>(group)]);

        auto* groupWidget = new QWidget(&dialog);
        auto* groupLayout = new QHBoxLayout(groupWidget);
        groupLayout->setContentsMargins(0, 0, 0, 0);
        groupLayout->addWidget(groupGainSpinBoxes[static_cast<size_t>(group)]);
        groupLayout->addWidget(groupPanSpinBoxes[static_cast<size_t>(group)]);
        groupLayout->addWidget(groupMuteCheckBoxes[static_cast<size_t>(group)]);
        groupLayout->addWidget(groupSoloCheckBoxes[static_cast<size_t>(group)]);
        layout->addRow(QStringLiteral("Group %1 gain/pan").arg(QChar(QLatin1Char('A' + group))), groupWidget);
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    outerLayout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    resamplerBackend_ = SampleLoader::resamplerBackendFromName(resamplerCombo->currentText());
    resamplerQuality_ = SampleLoader::resamplerQualityFromName(qualityCombo->currentText());
    beatDetectionScope_ = beatDetectionScopeFromName(beatScopeCombo->currentText());
    beatDetectionStartLengthSeconds_ = beatStartLengthSpinBox->value();
    beatDetectionEndLengthSeconds_ = beatEndLengthSpinBox->value();
    beatDetectionMergePolicy_ = beatDetectionMergePolicyFromName(beatMergeCombo->currentText());
    sampleNormalizationEnabled_ = normalizationCheckBox->isChecked();
    limiterEnabled_ = limiterCheckBox->isChecked();
    fadeTimeMs_ = fadeSpinBox->value();
    loopCrossfadeEnabled_ = crossfadeCheckBox->isChecked();
    loopCrossfadeMs_ = crossfadeSpinBox->value();
    masterGainPercent_ = masterGainSpinBox->value();
    for (int group = 0; group < 4; ++group) {
        groupGainPercent_[static_cast<size_t>(group)] = groupGainSpinBoxes[static_cast<size_t>(group)]->value();
        groupPanPercent_[static_cast<size_t>(group)] = groupPanSpinBoxes[static_cast<size_t>(group)]->value();
        groupMuted_[static_cast<size_t>(group)] = groupMuteCheckBoxes[static_cast<size_t>(group)]->isChecked();
        groupSolo_[static_cast<size_t>(group)] = groupSoloCheckBoxes[static_cast<size_t>(group)]->isChecked();
    }

    const QSignalBlocker masterBlocker(masterGainDial_);
    masterGainDial_->setValue(masterGainPercent_);
    applyMixerSettings();
    if (rootSample_ != nullptr) {
        queueRootTimingAnalysis(rootSample_);
    }
    for (Sample* sample : samplePoolWidget_->allSamples()) {
        if (sample != nullptr && sample != rootSample_ && sample->syncToRoot) {
            sample->timingAnalysisState = TimingAnalysisState::Unanalyzed;
            sample->hasTimingAnalysis = false;
            sample->hasBeatGrid = false;
            sample->detectedTempoBpm = 0.0;
            sample->syncedFromTempoBpm = 0.0;
            sample->currentTempoBpm = 0.0;
            updateSampleTimingStateForSync(sample);
        }
    }
    updateBottomStatus();
    saveSettings();
    statusBar()->showMessage(QStringLiteral("Updated SuperLooper audio settings."));
}

void MainWindow::setRecordingMethod(int index)
{
    const auto method = index == static_cast<int>(RecordingMethod::ArmedMiddlePedal)
        ? RecordingMethod::ArmedMiddlePedal
        : RecordingMethod::KeyPressesAutomatic;

    if (recordingMethod_ == method) {
        return;
    }

    if (activeMiddlePedalRecordingNoteIndex_ >= 0) {
        handleMiddlePedalReleased();
    }

    recordingMethod_ = method;
    armedRecordingNoteIndex_ = -1;
    statusBar()->showMessage(method == RecordingMethod::ArmedMiddlePedal
            ? QStringLiteral("Recording method: Armed / Middle Pedal. Press a key to arm it.")
            : QStringLiteral("Recording method: Key Presses / Automatic."));
}

void MainWindow::startRecordingForNote(int noteIndex)
{
    if (noteIndex < 0 || noteIndex >= PianoWidget::kKeyCount) {
        return;
    }

    recordPressTimes_[static_cast<size_t>(noteIndex)] = appClock_.elapsed();
    audioEngine_.startRecording(
        noteIndex,
        kMaxRecordSeconds,
        perKeyAutoMode_[static_cast<size_t>(noteIndex)]);
    statusBar()->showMessage(QStringLiteral("Recording %1").arg(noteNameForIndex(noteIndex)));
}

void MainWindow::finishRecordingForNote(int noteIndex)
{
    if (noteIndex < 0 || noteIndex >= PianoWidget::kKeyCount) {
        return;
    }

    const qint64 pressTime = recordPressTimes_[static_cast<size_t>(noteIndex)];
    const qint64 heldMs = appClock_.elapsed() - pressTime;
    if (heldMs >= kTapHoldThresholdMs) {
        audioEngine_.stopRecording(noteIndex);
        statusBar()->showMessage(QStringLiteral("Finishing held recording for %1").arg(noteNameForIndex(noteIndex)));
        return;
    }

    audioEngine_.setRecordingTarget(noteIndex, loopDurationSpinBox_->value());
    statusBar()->showMessage(QStringLiteral("Fixed-length recording for %1").arg(noteNameForIndex(noteIndex)));
}

void MainWindow::handleMiddlePedalPressed()
{
    if (mode_ != AppMode::Record || recordingMethod_ != RecordingMethod::ArmedMiddlePedal) {
        return;
    }

    if (activeMiddlePedalRecordingNoteIndex_ >= 0) {
        return;
    }

    if (armedRecordingNoteIndex_ < 0 || armedRecordingNoteIndex_ >= PianoWidget::kKeyCount) {
        statusBar()->showMessage(QStringLiteral("Press a piano key to arm recording before using the middle pedal."));
        return;
    }

    activeMiddlePedalRecordingNoteIndex_ = armedRecordingNoteIndex_;
    startRecordingForNote(activeMiddlePedalRecordingNoteIndex_);
}

void MainWindow::handleMiddlePedalReleased()
{
    if (recordingMethod_ != RecordingMethod::ArmedMiddlePedal || activeMiddlePedalRecordingNoteIndex_ < 0) {
        return;
    }

    const int noteIndex = activeMiddlePedalRecordingNoteIndex_;
    activeMiddlePedalRecordingNoteIndex_ = -1;
    finishRecordingForNote(noteIndex);
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    saveSettings();
    QMainWindow::closeEvent(event);
}

void MainWindow::updateModeLabel()
{
    QString background;
    switch (mode_) {
    case AppMode::Normal:
        background = QStringLiteral("#3f4850");
        break;
    case AppMode::Record:
        background = QStringLiteral("#9f3636");
        break;
    case AppMode::Playback:
        background = QStringLiteral("#2e7d57");
        break;
    case AppMode::Edit:
        background = QStringLiteral("#856404");
        break;
    }

    modeLabel_->setText(QStringLiteral("Mode: %1").arg(modeName(mode_)));
    modeLabel_->setStyleSheet(QStringLiteral(
        "QLabel {"
        " background: %1;"
        " color: white;"
        " border: 1px solid #1e2226;"
        " font-weight: 700;"
        " padding: 8px;"
        "}").arg(background));
}

void MainWindow::updateWaveformView(Sample* sample)
{
    if (sample == nullptr || sample->thumbnail.isNull()) {
        waveformLabel_->setPixmap(QPixmap());
        waveformLabel_->setText(QStringLiteral("Waveform View"));
        return;
    }

    waveformLabel_->setText(QString());
    waveformLabel_->setPixmap(sample->thumbnail.scaled(
        waveformLabel_->size().boundedTo(QSize(900, 160)),
        Qt::KeepAspectRatio,
        Qt::SmoothTransformation));
}

void MainWindow::updateSampleFlagControls(Sample* sample)
{
    updatingSampleFlagControls_ = true;
    const QSignalBlocker rootBlocker(rootCheckBox_);
    const QSignalBlocker syncBlocker(syncCheckBox_);
    rootCheckBox_->setEnabled(sample != nullptr);
    syncCheckBox_->setEnabled(sample != nullptr && sample != rootSample_);
    rootCheckBox_->setChecked(sample != nullptr && sample->isRoot);
    syncCheckBox_->setChecked(sample != nullptr && sample->syncToRoot);
    updatingSampleFlagControls_ = false;
}

void MainWindow::updateKeySettingsControls(int noteIndex)
{
    updatingKeySettingsControls_ = true;
    const QSignalBlocker blocker(keyAutoCheckBox_);
    const QSignalBlocker volumeBlocker(selectedKeyVolumeDial_);
    const QSignalBlocker panBlocker(selectedKeyPanDial_);
    const QSignalBlocker muteBlocker(selectedKeyMuteCheckBox_);
    const QSignalBlocker soloBlocker(selectedKeySoloCheckBox_);
    const QSignalBlocker selfMixBlocker(selectedKeySelfMixCheckBox_);
    const QSignalBlocker groupBlocker(selectedKeyGroupComboBox_);
    const bool valid = noteIndex >= 0 && noteIndex < PianoWidget::kKeyCount;
    const bool editMode = mode_ == AppMode::Edit;
    keyAutoCheckBox_->setEnabled(valid);
    selectedKeyLabel_->setText(valid ? noteNameForIndex(noteIndex) : QStringLiteral("Key --"));
    keyAutoCheckBox_->setChecked(valid && perKeyAutoMode_[static_cast<size_t>(noteIndex)]);
    selectedKeyVolumeDial_->setEnabled(valid && editMode);
    selectedKeyVolumeDial_->setValue(valid ? perKeyVolumePercent_[static_cast<size_t>(noteIndex)] : 100);
    selectedKeyPanDial_->setEnabled(valid && editMode);
    selectedKeyPanDial_->setValue(valid ? perKeyPanPercent_[static_cast<size_t>(noteIndex)] : 0);
    selectedKeyMuteCheckBox_->setEnabled(valid && editMode);
    selectedKeyMuteCheckBox_->setChecked(valid && perKeyMuted_[static_cast<size_t>(noteIndex)]);
    selectedKeySoloCheckBox_->setEnabled(valid && editMode);
    selectedKeySoloCheckBox_->setChecked(valid && perKeySolo_[static_cast<size_t>(noteIndex)]);
    selectedKeySelfMixCheckBox_->setEnabled(valid && editMode && perKeyLoopMode_[static_cast<size_t>(noteIndex)] == KeyLoopMode::NoLoop);
    selectedKeySelfMixCheckBox_->setChecked(valid && perKeySelfMixEnabled_[static_cast<size_t>(noteIndex)]);
    selectedKeyGroupComboBox_->setEnabled(valid && editMode);
    selectedKeyGroupComboBox_->setCurrentIndex(valid ? perKeyGroup_[static_cast<size_t>(noteIndex)] : 0);
    updatingKeySettingsControls_ = false;
    updateSelectedKeyControls(noteIndex);
}

void MainWindow::updateSelectedKeyControls(int noteIndex)
{
    const bool valid = noteIndex >= 0 && noteIndex < PianoWidget::kKeyCount;
    Sample* sample = valid ? assignedSamples_[static_cast<size_t>(noteIndex)] : nullptr;
    const bool editMode = mode_ == AppMode::Edit;
    selectedKeyPlayPauseButton_->setEnabled(editMode && valid && sample != nullptr);

    if (!editMode) {
        selectedKeySampleLabel_->setText(QStringLiteral("Edit mode selects keys"));
        selectedKeySampleLabel_->setToolTip(QString());
        return;
    }

    if (!valid) {
        selectedKeySampleLabel_->setText(QStringLiteral("No key selected"));
        selectedKeySampleLabel_->setToolTip(QString());
        return;
    }

    if (sample == nullptr) {
        selectedKeySampleLabel_->setText(QStringLiteral("No sample"));
        selectedKeySampleLabel_->setToolTip(QString());
        return;
    }

    const QString sourceText = sample->sourceFilePath.isEmpty()
        ? QStringLiteral("%1 (unsaved/memory)").arg(sample->name)
        : sample->sourceFilePath;
    const QString displayText = QStringLiteral("%1 | %2 | Self Mix %3 | Virtual Staccato %4")
        .arg(sourceText)
        .arg(loopModeName(perKeyLoopMode_[static_cast<size_t>(noteIndex)]))
        .arg(perKeySelfMixEnabled_[static_cast<size_t>(noteIndex)] ? QStringLiteral("On") : QStringLiteral("Off"))
        .arg(perKeyVirtualStaccatoEnabled_[static_cast<size_t>(noteIndex)] ? QStringLiteral("On") : QStringLiteral("Off"));
    selectedKeySampleLabel_->setText(QFontMetrics(selectedKeySampleLabel_->font()).elidedText(
        displayText,
        Qt::ElideMiddle,
        std::max(120, selectedKeySampleLabel_->width() - 10)));
    selectedKeySampleLabel_->setToolTip(displayText);
}

void MainWindow::assignSampleToKey(int noteIndex, Sample* sample)
{
    if (noteIndex < 0 || noteIndex >= PianoWidget::kKeyCount) {
        return;
    }

    assignedSamples_[static_cast<size_t>(noteIndex)] = sample;
    audioEngine_.assignSampleToKey(noteIndex, sample);
    audioEngine_.setKeyVolume(noteIndex, static_cast<float>(perKeyVolumePercent_[static_cast<size_t>(noteIndex)]) / 100.0F);
    audioEngine_.setKeyPan(noteIndex, static_cast<float>(perKeyPanPercent_[static_cast<size_t>(noteIndex)]) / 100.0F);
    audioEngine_.setKeyMuted(noteIndex, perKeyMuted_[static_cast<size_t>(noteIndex)]);
    audioEngine_.setKeySolo(noteIndex, perKeySolo_[static_cast<size_t>(noteIndex)]);
    audioEngine_.setKeyGroup(noteIndex, perKeyGroup_[static_cast<size_t>(noteIndex)]);
    audioEngine_.setKeyAttackMs(noteIndex, perKeyAttackMs_[static_cast<size_t>(noteIndex)]);
    audioEngine_.setKeyReleaseMs(noteIndex, perKeyReleaseMs_[static_cast<size_t>(noteIndex)]);
    audioEngine_.setKeyStaccato(noteIndex, static_cast<float>(perKeyStaccatoPercent_[static_cast<size_t>(noteIndex)]) / 100.0F);
    audioEngine_.setKeyLoopMode(noteIndex, perKeyLoopMode_[static_cast<size_t>(noteIndex)]);
    audioEngine_.setKeyVirtualStaccatoEnabled(noteIndex, perKeyVirtualStaccatoEnabled_[static_cast<size_t>(noteIndex)]);
    audioEngine_.setKeySelfMixEnabled(noteIndex, perKeySelfMixEnabled_[static_cast<size_t>(noteIndex)]);
    audioEngine_.setKeyTrimRange(noteIndex, perKeyTrimStart_[static_cast<size_t>(noteIndex)], perKeyTrimEnd_[static_cast<size_t>(noteIndex)]);
    audioEngine_.setKeyPlaybackRate(noteIndex, perKeyPlaybackRate_[static_cast<size_t>(noteIndex)]);
    for (size_t slotIndex = 0; slotIndex < AudioEngine::kLv2ChainSize; ++slotIndex) {
        QString ignoredError;
        audioEngine_.setKeyLv2SlotUri(noteIndex, slotIndex, perKeyLv2PluginUris_[static_cast<size_t>(noteIndex)][slotIndex], &ignoredError);
        audioEngine_.setKeyLv2SlotEnabled(noteIndex, slotIndex, perKeyLv2Enabled_[static_cast<size_t>(noteIndex)][slotIndex]);
        audioEngine_.setKeyLv2SlotParameterValues(noteIndex, slotIndex, perKeyLv2ParameterValues_[static_cast<size_t>(noteIndex)][slotIndex]);
    }
    if (selectedNoteIndex_ == noteIndex) {
        updateSelectedKeyControls(noteIndex);
    }
}

void MainWindow::loadSettings()
{
    QSettings settings(QStringLiteral("RtAudioLinux"), QStringLiteral("SuperLooper"));
    QSettings legacySettings(QStringLiteral("JackLooper"), QStringLiteral("JackLooper"));
    restoreGeometry(settingsValueWithLegacyFallback(settings, legacySettings, QStringLiteral("mainWindow/geometry")).toByteArray());
    loopDurationSpinBox_->setValue(settingsValueWithLegacyFallback(settings, legacySettings, QStringLiteral("recording/loopDurationSec"), 4.0).toDouble());
    const int recordingMethod = settingsValueWithLegacyFallback(settings, legacySettings, QStringLiteral("recording/method"), 0).toInt();
    recordingMethodComboBox_->setCurrentIndex(recordingMethod == 1 ? 1 : 0);
    setRecordingMethod(recordingMethodComboBox_->currentIndex());
    barsPerRootSpinBox_->setValue(settingsValueWithLegacyFallback(settings, legacySettings, QStringLiteral("sync/barsPerRoot"), 1).toInt());
    resamplerBackend_ = SampleLoader::resamplerBackendFromName(settingsValueWithLegacyFallback(settings, legacySettings, QStringLiteral("audio/resampler"), QStringLiteral("Automatic")).toString());
    resamplerQuality_ = SampleLoader::resamplerQualityFromName(settingsValueWithLegacyFallback(
        settings,
        legacySettings,
        QStringLiteral("audio/resamplerQuality"),
        QStringLiteral("Highest Available")).toString());
    beatDetectionScope_ = beatDetectionScopeFromName(settingsValueWithLegacyFallback(
        settings,
        legacySettings,
        QStringLiteral("audio/beatDetectionScope"),
        QStringLiteral("Detect from beginning")).toString());
    beatDetectionMergePolicy_ = beatDetectionMergePolicyFromName(settingsValueWithLegacyFallback(
        settings,
        legacySettings,
        QStringLiteral("audio/beatDetectionMergePolicy"),
        QStringLiteral("Prefer higher confidence")).toString());
    const double legacyWindowSeconds = std::clamp(settingsValueWithLegacyFallback(
        settings,
        legacySettings,
        QStringLiteral("audio/beatDetectionWindowSeconds"),
        20.0).toDouble(), 5.0, 120.0);
    beatDetectionStartLengthSeconds_ = std::clamp(settingsValueWithLegacyFallback(
        settings,
        legacySettings,
        QStringLiteral("audio/beatDetectionStartLengthSeconds"),
        legacyWindowSeconds).toDouble(), 5.0, 120.0);
    beatDetectionEndLengthSeconds_ = std::clamp(settingsValueWithLegacyFallback(
        settings,
        legacySettings,
        QStringLiteral("audio/beatDetectionEndLengthSeconds"),
        legacyWindowSeconds).toDouble(), 5.0, 120.0);
    sampleNormalizationEnabled_ = settingsValueWithLegacyFallback(settings, legacySettings, QStringLiteral("audio/sampleNormalizationEnabled"), false).toBool();
    limiterEnabled_ = settingsValueWithLegacyFallback(settings, legacySettings, QStringLiteral("audio/limiterEnabled"), true).toBool();
    fadeTimeMs_ = settingsValueWithLegacyFallback(settings, legacySettings, QStringLiteral("audio/fadeTimeMs"), 5.0).toDouble();
    loopCrossfadeEnabled_ = settingsValueWithLegacyFallback(settings, legacySettings, QStringLiteral("audio/loopCrossfadeEnabled"), false).toBool();
    loopCrossfadeMs_ = settingsValueWithLegacyFallback(settings, legacySettings, QStringLiteral("audio/loopCrossfadeMs"), 10.0).toDouble();
    masterGainPercent_ = std::clamp(settingsValueWithLegacyFallback(settings, legacySettings, QStringLiteral("audio/masterGainPercent"), 100).toInt(), 0, 200);
    masterLv2PluginUris_.fill(QString());
    masterLv2Enabled_.fill(true);
    for (std::vector<float>& values : masterLv2ParameterValues_) {
        values.clear();
    }
    masterLv2PluginUris_[0] = settingsValueWithLegacyFallback(settings, legacySettings, QStringLiteral("audio/masterLv2PluginUri"), QString()).toString();
    masterLv2ParameterValues_[0] = floatVectorFromVariant(settingsValueWithLegacyFallback(
        settings,
        legacySettings,
        QStringLiteral("audio/masterLv2Parameters"),
        QVariantList()));
    for (size_t slotIndex = 0; slotIndex < AudioEngine::kLv2ChainSize; ++slotIndex) {
        masterLv2PluginUris_[slotIndex] = settingsValueWithLegacyFallback(
            settings,
            legacySettings,
            QStringLiteral("audio/masterLv2Slots/%1/uri").arg(slotIndex),
            masterLv2PluginUris_[slotIndex]).toString();
        masterLv2Enabled_[slotIndex] = settingsValueWithLegacyFallback(
            settings,
            legacySettings,
            QStringLiteral("audio/masterLv2Slots/%1/enabled").arg(slotIndex),
            true).toBool();
        masterLv2ParameterValues_[slotIndex] = floatVectorFromVariant(settingsValueWithLegacyFallback(
            settings,
            legacySettings,
            QStringLiteral("audio/masterLv2Slots/%1/parameters").arg(slotIndex),
            slotIndex == 0 ? QVariant(variantListFromFloatVector(masterLv2ParameterValues_[0])) : QVariant(QVariantList())));
    }
    masterGainDial_->setValue(masterGainPercent_);

    for (int noteIndex = 0; noteIndex < PianoWidget::kKeyCount; ++noteIndex) {
        perKeyAutoMode_[static_cast<size_t>(noteIndex)] =
            settingsValueWithLegacyFallback(settings, legacySettings, QStringLiteral("perKeyAuto/%1").arg(noteIndex), false).toBool();
    }

    for (int noteIndex = 0; noteIndex < PianoWidget::kKeyCount; ++noteIndex) {
        const int volume = settingsValueWithLegacyFallback(settings, legacySettings, QStringLiteral("perKeyVolume/%1").arg(noteIndex), 100).toInt();
        perKeyVolumePercent_[static_cast<size_t>(noteIndex)] = std::clamp(volume, 0, 200);
        audioEngine_.setKeyVolume(noteIndex, static_cast<float>(perKeyVolumePercent_[static_cast<size_t>(noteIndex)]) / 100.0F);
    }

    for (int noteIndex = 0; noteIndex < PianoWidget::kKeyCount; ++noteIndex) {
        perKeyPanPercent_[static_cast<size_t>(noteIndex)] = std::clamp(
            settingsValueWithLegacyFallback(settings, legacySettings, QStringLiteral("perKeyPan/%1").arg(noteIndex), 0).toInt(),
            -100,
            100);
    }

    for (int noteIndex = 0; noteIndex < PianoWidget::kKeyCount; ++noteIndex) {
        perKeyMuted_[static_cast<size_t>(noteIndex)] =
            settingsValueWithLegacyFallback(settings, legacySettings, QStringLiteral("perKeyMute/%1").arg(noteIndex), false).toBool();
    }

    for (int noteIndex = 0; noteIndex < PianoWidget::kKeyCount; ++noteIndex) {
        perKeySolo_[static_cast<size_t>(noteIndex)] =
            settingsValueWithLegacyFallback(settings, legacySettings, QStringLiteral("perKeySolo/%1").arg(noteIndex), false).toBool();
    }

    for (int noteIndex = 0; noteIndex < PianoWidget::kKeyCount; ++noteIndex) {
        perKeyGroup_[static_cast<size_t>(noteIndex)] = std::clamp(
            settingsValueWithLegacyFallback(settings, legacySettings, QStringLiteral("perKeyGroup/%1").arg(noteIndex), 0).toInt(),
            0,
            3);
    }

    for (int noteIndex = 0; noteIndex < PianoWidget::kKeyCount; ++noteIndex) {
        perKeyAttackMs_[static_cast<size_t>(noteIndex)] = std::clamp(
            settingsValueWithLegacyFallback(settings, legacySettings, QStringLiteral("perKeyAttack/%1").arg(noteIndex), 0.0).toDouble(),
            0.0,
            5000.0);
        perKeyReleaseMs_[static_cast<size_t>(noteIndex)] = std::clamp(
            settingsValueWithLegacyFallback(settings, legacySettings, QStringLiteral("perKeyRelease/%1").arg(noteIndex), 0.0).toDouble(),
            0.0,
            5000.0);
        perKeyStaccatoPercent_[static_cast<size_t>(noteIndex)] = std::clamp(
            settingsValueWithLegacyFallback(settings, legacySettings, QStringLiteral("perKeyStaccato/%1").arg(noteIndex), 0).toInt(),
            0,
            100);
        perKeyLoopMode_[static_cast<size_t>(noteIndex)] =
            settingsValueWithLegacyFallback(settings, legacySettings, QStringLiteral("perKeyLoopMode/%1").arg(noteIndex), QStringLiteral("Loop")).toString()
                == QStringLiteral("No Loop")
            ? KeyLoopMode::NoLoop
            : KeyLoopMode::Loop;
        perKeyVirtualStaccatoEnabled_[static_cast<size_t>(noteIndex)] =
            settingsValueWithLegacyFallback(
                settings,
                legacySettings,
                QStringLiteral("perKeyVirtualStaccato/%1").arg(noteIndex),
                perKeyLoopMode_[static_cast<size_t>(noteIndex)] == KeyLoopMode::NoLoop).toBool();
        perKeySelfMixEnabled_[static_cast<size_t>(noteIndex)] =
            settingsValueWithLegacyFallback(settings, legacySettings, QStringLiteral("perKeySelfMix/%1").arg(noteIndex), false).toBool();
        perKeyTrimStart_[static_cast<size_t>(noteIndex)] = std::clamp(
            settingsValueWithLegacyFallback(settings, legacySettings, QStringLiteral("perKeyTrimStart/%1").arg(noteIndex), 0.0).toDouble(),
            0.0,
            1.0);
        perKeyTrimEnd_[static_cast<size_t>(noteIndex)] = std::clamp(
            settingsValueWithLegacyFallback(settings, legacySettings, QStringLiteral("perKeyTrimEnd/%1").arg(noteIndex), 1.0).toDouble(),
            perKeyTrimStart_[static_cast<size_t>(noteIndex)],
            1.0);
        perKeyPlaybackRate_[static_cast<size_t>(noteIndex)] = std::clamp(
            settingsValueWithLegacyFallback(settings, legacySettings, QStringLiteral("perKeyPlaybackRate/%1").arg(noteIndex), 1.0).toDouble(),
            0.0625,
            16.0);
        perKeyLv2PluginUris_[static_cast<size_t>(noteIndex)].fill(QString());
        perKeyLv2Enabled_[static_cast<size_t>(noteIndex)].fill(true);
        for (std::vector<float>& values : perKeyLv2ParameterValues_[static_cast<size_t>(noteIndex)]) {
            values.clear();
        }
        perKeyLv2PluginUris_[static_cast<size_t>(noteIndex)][0] =
            settingsValueWithLegacyFallback(settings, legacySettings, QStringLiteral("perKeyLv2Plugin/%1").arg(noteIndex), QString()).toString();
        perKeyLv2ParameterValues_[static_cast<size_t>(noteIndex)][0] = floatVectorFromVariant(
            settingsValueWithLegacyFallback(
                settings,
                legacySettings,
                QStringLiteral("perKeyLv2Parameters/%1").arg(noteIndex),
                QVariantList()));
        for (size_t slotIndex = 0; slotIndex < AudioEngine::kLv2ChainSize; ++slotIndex) {
            perKeyLv2PluginUris_[static_cast<size_t>(noteIndex)][slotIndex] =
                settingsValueWithLegacyFallback(
                    settings,
                    legacySettings,
                    QStringLiteral("perKeyLv2Slots/%1/%2/uri").arg(noteIndex).arg(slotIndex),
                    perKeyLv2PluginUris_[static_cast<size_t>(noteIndex)][slotIndex]).toString();
            perKeyLv2Enabled_[static_cast<size_t>(noteIndex)][slotIndex] =
                settingsValueWithLegacyFallback(
                    settings,
                    legacySettings,
                    QStringLiteral("perKeyLv2Slots/%1/%2/enabled").arg(noteIndex).arg(slotIndex),
                    true).toBool();
            perKeyLv2ParameterValues_[static_cast<size_t>(noteIndex)][slotIndex] = floatVectorFromVariant(
                settingsValueWithLegacyFallback(
                    settings,
                    legacySettings,
                    QStringLiteral("perKeyLv2Slots/%1/%2/parameters").arg(noteIndex).arg(slotIndex),
                    slotIndex == 0
                        ? QVariant(variantListFromFloatVector(perKeyLv2ParameterValues_[static_cast<size_t>(noteIndex)][0]))
                        : QVariant(QVariantList())));
        }
    }

    for (int group = 0; group < 4; ++group) {
        const QString prefix = QString::number(group);
        groupGainPercent_[static_cast<size_t>(group)] = std::clamp(settingsValueWithLegacyFallback(settings, legacySettings, QStringLiteral("groups/%1/gain").arg(prefix), 100).toInt(), 0, 200);
        groupPanPercent_[static_cast<size_t>(group)] = std::clamp(settingsValueWithLegacyFallback(settings, legacySettings, QStringLiteral("groups/%1/pan").arg(prefix), 0).toInt(), -100, 100);
        groupMuted_[static_cast<size_t>(group)] = settingsValueWithLegacyFallback(settings, legacySettings, QStringLiteral("groups/%1/muted").arg(prefix), false).toBool();
        groupSolo_[static_cast<size_t>(group)] = settingsValueWithLegacyFallback(settings, legacySettings, QStringLiteral("groups/%1/solo").arg(prefix), false).toBool();
    }
    applyMixerSettings();
    updateKeySettingsControls(selectedNoteIndex_);
}

void MainWindow::saveSettings() const
{
    QSettings settings(QStringLiteral("RtAudioLinux"), QStringLiteral("SuperLooper"));
    settings.setValue(QStringLiteral("mainWindow/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("recording/loopDurationSec"), loopDurationSpinBox_->value());
    settings.setValue(QStringLiteral("recording/method"), static_cast<int>(recordingMethod_));
    settings.setValue(QStringLiteral("sync/barsPerRoot"), barsPerRootSpinBox_->value());
    settings.setValue(QStringLiteral("audio/resampler"), SampleLoader::resamplerBackendName(resamplerBackend_));
    settings.setValue(QStringLiteral("audio/resamplerQuality"), SampleLoader::resamplerQualityName(resamplerQuality_));
    settings.setValue(QStringLiteral("audio/beatDetectionScope"), beatDetectionScopeName(beatDetectionScope_));
    settings.setValue(QStringLiteral("audio/beatDetectionMergePolicy"), beatDetectionMergePolicyName(beatDetectionMergePolicy_));
    settings.setValue(QStringLiteral("audio/beatDetectionStartLengthSeconds"), beatDetectionStartLengthSeconds_);
    settings.setValue(QStringLiteral("audio/beatDetectionEndLengthSeconds"), beatDetectionEndLengthSeconds_);
    settings.setValue(QStringLiteral("audio/sampleNormalizationEnabled"), sampleNormalizationEnabled_);
    settings.setValue(QStringLiteral("audio/limiterEnabled"), limiterEnabled_);
    settings.setValue(QStringLiteral("audio/fadeTimeMs"), fadeTimeMs_);
    settings.setValue(QStringLiteral("audio/loopCrossfadeEnabled"), loopCrossfadeEnabled_);
    settings.setValue(QStringLiteral("audio/loopCrossfadeMs"), loopCrossfadeMs_);
    settings.setValue(QStringLiteral("audio/masterGainPercent"), masterGainPercent_);
    settings.setValue(QStringLiteral("audio/masterLv2PluginUri"), masterLv2PluginUris_[0]);
    settings.setValue(QStringLiteral("audio/masterLv2Parameters"), variantListFromFloatVector(masterLv2ParameterValues_[0]));
    for (size_t slotIndex = 0; slotIndex < AudioEngine::kLv2ChainSize; ++slotIndex) {
        settings.setValue(QStringLiteral("audio/masterLv2Slots/%1/uri").arg(slotIndex), masterLv2PluginUris_[slotIndex]);
        settings.setValue(QStringLiteral("audio/masterLv2Slots/%1/enabled").arg(slotIndex), masterLv2Enabled_[slotIndex]);
        settings.setValue(QStringLiteral("audio/masterLv2Slots/%1/parameters").arg(slotIndex), variantListFromFloatVector(masterLv2ParameterValues_[slotIndex]));
    }

    settings.beginGroup(QStringLiteral("perKeyAuto"));
    for (int noteIndex = 0; noteIndex < PianoWidget::kKeyCount; ++noteIndex) {
        settings.setValue(QString::number(noteIndex), perKeyAutoMode_[static_cast<size_t>(noteIndex)]);
    }
    settings.endGroup();

    settings.beginGroup(QStringLiteral("perKeyVolume"));
    for (int noteIndex = 0; noteIndex < PianoWidget::kKeyCount; ++noteIndex) {
        settings.setValue(QString::number(noteIndex), perKeyVolumePercent_[static_cast<size_t>(noteIndex)]);
    }
    settings.endGroup();

    settings.beginGroup(QStringLiteral("perKeyPan"));
    for (int noteIndex = 0; noteIndex < PianoWidget::kKeyCount; ++noteIndex) {
        settings.setValue(QString::number(noteIndex), perKeyPanPercent_[static_cast<size_t>(noteIndex)]);
    }
    settings.endGroup();

    settings.beginGroup(QStringLiteral("perKeyMute"));
    for (int noteIndex = 0; noteIndex < PianoWidget::kKeyCount; ++noteIndex) {
        settings.setValue(QString::number(noteIndex), perKeyMuted_[static_cast<size_t>(noteIndex)]);
    }
    settings.endGroup();

    settings.beginGroup(QStringLiteral("perKeySolo"));
    for (int noteIndex = 0; noteIndex < PianoWidget::kKeyCount; ++noteIndex) {
        settings.setValue(QString::number(noteIndex), perKeySolo_[static_cast<size_t>(noteIndex)]);
    }
    settings.endGroup();

    settings.beginGroup(QStringLiteral("perKeyGroup"));
    for (int noteIndex = 0; noteIndex < PianoWidget::kKeyCount; ++noteIndex) {
        settings.setValue(QString::number(noteIndex), perKeyGroup_[static_cast<size_t>(noteIndex)]);
    }
    settings.endGroup();

    settings.beginGroup(QStringLiteral("perKeyAttack"));
    for (int noteIndex = 0; noteIndex < PianoWidget::kKeyCount; ++noteIndex) {
        settings.setValue(QString::number(noteIndex), perKeyAttackMs_[static_cast<size_t>(noteIndex)]);
    }
    settings.endGroup();

    settings.beginGroup(QStringLiteral("perKeyRelease"));
    for (int noteIndex = 0; noteIndex < PianoWidget::kKeyCount; ++noteIndex) {
        settings.setValue(QString::number(noteIndex), perKeyReleaseMs_[static_cast<size_t>(noteIndex)]);
    }
    settings.endGroup();

    settings.beginGroup(QStringLiteral("perKeyStaccato"));
    for (int noteIndex = 0; noteIndex < PianoWidget::kKeyCount; ++noteIndex) {
        settings.setValue(QString::number(noteIndex), perKeyStaccatoPercent_[static_cast<size_t>(noteIndex)]);
    }
    settings.endGroup();

    settings.beginGroup(QStringLiteral("perKeyLoopMode"));
    for (int noteIndex = 0; noteIndex < PianoWidget::kKeyCount; ++noteIndex) {
        settings.setValue(QString::number(noteIndex), loopModeName(perKeyLoopMode_[static_cast<size_t>(noteIndex)]));
    }
    settings.endGroup();

    settings.beginGroup(QStringLiteral("perKeyVirtualStaccato"));
    for (int noteIndex = 0; noteIndex < PianoWidget::kKeyCount; ++noteIndex) {
        settings.setValue(QString::number(noteIndex), perKeyVirtualStaccatoEnabled_[static_cast<size_t>(noteIndex)]);
    }
    settings.endGroup();

    settings.beginGroup(QStringLiteral("perKeySelfMix"));
    for (int noteIndex = 0; noteIndex < PianoWidget::kKeyCount; ++noteIndex) {
        settings.setValue(QString::number(noteIndex), perKeySelfMixEnabled_[static_cast<size_t>(noteIndex)]);
    }
    settings.endGroup();

    settings.beginGroup(QStringLiteral("perKeyTrimStart"));
    for (int noteIndex = 0; noteIndex < PianoWidget::kKeyCount; ++noteIndex) {
        settings.setValue(QString::number(noteIndex), perKeyTrimStart_[static_cast<size_t>(noteIndex)]);
    }
    settings.endGroup();

    settings.beginGroup(QStringLiteral("perKeyTrimEnd"));
    for (int noteIndex = 0; noteIndex < PianoWidget::kKeyCount; ++noteIndex) {
        settings.setValue(QString::number(noteIndex), perKeyTrimEnd_[static_cast<size_t>(noteIndex)]);
    }
    settings.endGroup();

    settings.beginGroup(QStringLiteral("perKeyPlaybackRate"));
    for (int noteIndex = 0; noteIndex < PianoWidget::kKeyCount; ++noteIndex) {
        settings.setValue(QString::number(noteIndex), perKeyPlaybackRate_[static_cast<size_t>(noteIndex)]);
    }
    settings.endGroup();

    settings.beginGroup(QStringLiteral("perKeyLv2Plugin"));
    for (int noteIndex = 0; noteIndex < PianoWidget::kKeyCount; ++noteIndex) {
        settings.setValue(QString::number(noteIndex), perKeyLv2PluginUris_[static_cast<size_t>(noteIndex)][0]);
    }
    settings.endGroup();

    settings.beginGroup(QStringLiteral("perKeyLv2Parameters"));
    for (int noteIndex = 0; noteIndex < PianoWidget::kKeyCount; ++noteIndex) {
        settings.setValue(QString::number(noteIndex), variantListFromFloatVector(perKeyLv2ParameterValues_[static_cast<size_t>(noteIndex)][0]));
    }
    settings.endGroup();

    for (int noteIndex = 0; noteIndex < PianoWidget::kKeyCount; ++noteIndex) {
        for (size_t slotIndex = 0; slotIndex < AudioEngine::kLv2ChainSize; ++slotIndex) {
            settings.setValue(
                QStringLiteral("perKeyLv2Slots/%1/%2/uri").arg(noteIndex).arg(slotIndex),
                perKeyLv2PluginUris_[static_cast<size_t>(noteIndex)][slotIndex]);
            settings.setValue(
                QStringLiteral("perKeyLv2Slots/%1/%2/enabled").arg(noteIndex).arg(slotIndex),
                perKeyLv2Enabled_[static_cast<size_t>(noteIndex)][slotIndex]);
            settings.setValue(
                QStringLiteral("perKeyLv2Slots/%1/%2/parameters").arg(noteIndex).arg(slotIndex),
                variantListFromFloatVector(perKeyLv2ParameterValues_[static_cast<size_t>(noteIndex)][slotIndex]));
        }
    }

    settings.beginGroup(QStringLiteral("groups"));
    for (int group = 0; group < 4; ++group) {
        const QString prefix = QString::number(group);
        settings.setValue(prefix + QStringLiteral("/gain"), groupGainPercent_[static_cast<size_t>(group)]);
        settings.setValue(prefix + QStringLiteral("/pan"), groupPanPercent_[static_cast<size_t>(group)]);
        settings.setValue(prefix + QStringLiteral("/muted"), groupMuted_[static_cast<size_t>(group)]);
        settings.setValue(prefix + QStringLiteral("/solo"), groupSolo_[static_cast<size_t>(group)]);
    }
    settings.endGroup();
}

void MainWindow::applyMixerSettings()
{
    samplePoolWidget_->setResamplerBackend(resamplerBackend_);
    samplePoolWidget_->setResamplerQuality(resamplerQuality_);
    samplePoolWidget_->setApplyNormalization(sampleNormalizationEnabled_);
    audioEngine_.setMasterGain(static_cast<float>(masterGainPercent_) / 100.0F);
    audioEngine_.setLimiterEnabled(limiterEnabled_);
    audioEngine_.setSampleNormalizationEnabled(sampleNormalizationEnabled_);
    audioEngine_.setFadeTimeMs(fadeTimeMs_);
    audioEngine_.setLoopCrossfade(loopCrossfadeEnabled_, loopCrossfadeMs_);

    for (int noteIndex = 0; noteIndex < PianoWidget::kKeyCount; ++noteIndex) {
        audioEngine_.setKeyVolume(noteIndex, static_cast<float>(perKeyVolumePercent_[static_cast<size_t>(noteIndex)]) / 100.0F);
        audioEngine_.setKeyPan(noteIndex, static_cast<float>(perKeyPanPercent_[static_cast<size_t>(noteIndex)]) / 100.0F);
        audioEngine_.setKeyMuted(noteIndex, perKeyMuted_[static_cast<size_t>(noteIndex)]);
        audioEngine_.setKeySolo(noteIndex, perKeySolo_[static_cast<size_t>(noteIndex)]);
        audioEngine_.setKeyGroup(noteIndex, perKeyGroup_[static_cast<size_t>(noteIndex)]);
        audioEngine_.setKeyAttackMs(noteIndex, perKeyAttackMs_[static_cast<size_t>(noteIndex)]);
        audioEngine_.setKeyReleaseMs(noteIndex, perKeyReleaseMs_[static_cast<size_t>(noteIndex)]);
        audioEngine_.setKeyStaccato(noteIndex, static_cast<float>(perKeyStaccatoPercent_[static_cast<size_t>(noteIndex)]) / 100.0F);
        audioEngine_.setKeyLoopMode(noteIndex, perKeyLoopMode_[static_cast<size_t>(noteIndex)]);
        audioEngine_.setKeyVirtualStaccatoEnabled(noteIndex, perKeyVirtualStaccatoEnabled_[static_cast<size_t>(noteIndex)]);
        audioEngine_.setKeySelfMixEnabled(noteIndex, perKeySelfMixEnabled_[static_cast<size_t>(noteIndex)]);
        audioEngine_.setKeyTrimRange(noteIndex, perKeyTrimStart_[static_cast<size_t>(noteIndex)], perKeyTrimEnd_[static_cast<size_t>(noteIndex)]);
        audioEngine_.setKeyPlaybackRate(noteIndex, perKeyPlaybackRate_[static_cast<size_t>(noteIndex)]);
        for (size_t slotIndex = 0; slotIndex < AudioEngine::kLv2ChainSize; ++slotIndex) {
            QString ignoredError;
            audioEngine_.setKeyLv2SlotUri(noteIndex, slotIndex, perKeyLv2PluginUris_[static_cast<size_t>(noteIndex)][slotIndex], &ignoredError);
            audioEngine_.setKeyLv2SlotEnabled(noteIndex, slotIndex, perKeyLv2Enabled_[static_cast<size_t>(noteIndex)][slotIndex]);
            audioEngine_.setKeyLv2SlotParameterValues(noteIndex, slotIndex, perKeyLv2ParameterValues_[static_cast<size_t>(noteIndex)][slotIndex]);
        }
    }

    for (int group = 0; group < 4; ++group) {
        audioEngine_.setGroupGain(group, static_cast<float>(groupGainPercent_[static_cast<size_t>(group)]) / 100.0F);
        audioEngine_.setGroupPan(group, static_cast<float>(groupPanPercent_[static_cast<size_t>(group)]) / 100.0F);
        audioEngine_.setGroupMuted(group, groupMuted_[static_cast<size_t>(group)]);
        audioEngine_.setGroupSolo(group, groupSolo_[static_cast<size_t>(group)]);
    }

    for (size_t slotIndex = 0; slotIndex < AudioEngine::kLv2ChainSize; ++slotIndex) {
        QString ignoredError;
        audioEngine_.setMasterLv2SlotUri(slotIndex, masterLv2PluginUris_[slotIndex], &ignoredError);
        audioEngine_.setMasterLv2SlotEnabled(slotIndex, masterLv2Enabled_[slotIndex]);
        audioEngine_.setMasterLv2SlotParameterValues(slotIndex, masterLv2ParameterValues_[slotIndex]);
    }
}

void MainWindow::showOptimizationSummary()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("SuperLooper Optimization Summary"));
    dialog.resize(760, 520);
    auto* layout = new QVBoxLayout(&dialog);
    auto* textEdit = new QPlainTextEdit(&dialog);
    textEdit->setReadOnly(true);
    textEdit->setPlainText(OptimizationLog::readAll());
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(textEdit);
    layout->addWidget(buttons);
    dialog.exec();
}

bool MainWindow::editLv2Parameters(const QString& pluginUri, const QString& title, std::vector<float>* values)
{
    if (values == nullptr) {
        return false;
    }

    const std::optional<Lv2PluginInfo> pluginInfo = Lv2Host::pluginInfoForUri(pluginUri);
    if (!pluginInfo.has_value()) {
        QMessageBox::warning(this, title, QStringLiteral("Could not load LV2 plugin metadata."));
        return false;
    }

    if (pluginInfo->controlInputs.empty()) {
        QMessageBox::information(this, title, QStringLiteral("This LV2 plugin exposes no editable control inputs."));
        return false;
    }

    std::vector<float> workingValues = *values;
    if (workingValues.size() < pluginInfo->controlInputs.size()) {
        workingValues.resize(pluginInfo->controlInputs.size());
        for (size_t index = 0; index < pluginInfo->controlInputs.size(); ++index) {
            if (index >= values->size()) {
                workingValues[index] = pluginInfo->controlInputs[index].defaultValue;
            }
        }
    }

    QDialog dialog(this);
    dialog.setWindowTitle(title);
    dialog.resize(520, 440);
    auto* layout = new QVBoxLayout(&dialog);
    auto* descriptionLabel = new QLabel(
        QStringLiteral("%1\n%2")
            .arg(pluginInfo->name)
            .arg(pluginInfo->uri),
        &dialog);
    descriptionLabel->setWordWrap(true);
    layout->addWidget(descriptionLabel);

    auto* scrollArea = new QScrollArea(&dialog);
    scrollArea->setWidgetResizable(true);
    auto* container = new QWidget(scrollArea);
    auto* formLayout = new QFormLayout(container);
    formLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    std::vector<QDoubleSpinBox*> editors;
    editors.reserve(pluginInfo->controlInputs.size());
    for (size_t index = 0; index < pluginInfo->controlInputs.size(); ++index) {
        const Lv2ControlPortInfo& port = pluginInfo->controlInputs[index];
        auto* editor = new QDoubleSpinBox(container);
        editor->setDecimals(4);
        editor->setRange(port.minimumValue, port.maximumValue);
        editor->setSingleStep(std::max(0.001, (port.maximumValue - port.minimumValue) / 100.0));
        editor->setValue(std::clamp(
            static_cast<double>(workingValues[index]),
            static_cast<double>(port.minimumValue),
            static_cast<double>(port.maximumValue)));
        editor->setToolTip(QStringLiteral("%1 [%2]\nDefault: %3\nRange: %4 to %5")
            .arg(port.name, port.symbol)
            .arg(port.defaultValue, 0, 'f', 4)
            .arg(port.minimumValue, 0, 'f', 4)
            .arg(port.maximumValue, 0, 'f', 4));
        formLayout->addRow(QStringLiteral("%1 (%2)").arg(port.name, port.symbol), editor);
        editors.push_back(editor);
    }

    scrollArea->setWidget(container);
    layout->addWidget(scrollArea);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults,
        &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (QPushButton* defaultsButton = buttons->button(QDialogButtonBox::RestoreDefaults); defaultsButton != nullptr) {
        connect(defaultsButton, &QPushButton::clicked, &dialog, [editors, pluginInfo]() {
            for (size_t index = 0; index < editors.size(); ++index) {
                editors[index]->setValue(pluginInfo->controlInputs[index].defaultValue);
            }
        });
    }
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }

    values->resize(editors.size());
    for (size_t index = 0; index < editors.size(); ++index) {
        (*values)[index] = static_cast<float>(editors[index]->value());
    }
    return true;
}

bool MainWindow::editLv2Chain(
    const QString& title,
    Lv2SlotUriArray* uris,
    Lv2SlotEnabledArray* enabled,
    Lv2SlotParameterArray* parameterValues)
{
    if (uris == nullptr || enabled == nullptr || parameterValues == nullptr) {
        return false;
    }

    Lv2SlotUriArray workingUris = *uris;
    Lv2SlotEnabledArray workingEnabled = *enabled;
    Lv2SlotParameterArray workingParameters = *parameterValues;

    QDialog dialog(this);
    dialog.setWindowTitle(title);
    dialog.resize(760, 420);
    auto* layout = new QVBoxLayout(&dialog);
    auto* scrollArea = new QScrollArea(&dialog);
    scrollArea->setWidgetResizable(true);
    auto* container = new QWidget(scrollArea);
    auto* grid = new QGridLayout(container);
    grid->setColumnStretch(1, 1);
    grid->addWidget(new QLabel(QStringLiteral("Slot"), container), 0, 0);
    grid->addWidget(new QLabel(QStringLiteral("Plugin"), container), 0, 1);
    grid->addWidget(new QLabel(QStringLiteral("Enabled"), container), 0, 2);

    std::array<QLabel*, AudioEngine::kLv2ChainSize> pluginLabels {};
    std::array<QCheckBox*, AudioEngine::kLv2ChainSize> enabledBoxes {};
    std::array<QPushButton*, AudioEngine::kLv2ChainSize> parameterButtons {};

    auto refreshSlot = [&](size_t slotIndex) {
        const QString& uri = workingUris[slotIndex];
        pluginLabels[slotIndex]->setText(uri.isEmpty()
                ? QStringLiteral("None")
                : Lv2Host::displayNameForUri(uri));
        pluginLabels[slotIndex]->setToolTip(uri);
        parameterButtons[slotIndex]->setEnabled(!uri.isEmpty());
    };

    for (size_t slotIndex = 0; slotIndex < AudioEngine::kLv2ChainSize; ++slotIndex) {
        auto* slotLabel = new QLabel(QStringLiteral("%1").arg(slotIndex + 1), container);
        auto* pluginLabel = new QLabel(container);
        pluginLabel->setFrameShape(QFrame::StyledPanel);
        auto* enableBox = new QCheckBox(QStringLiteral("On"), container);
        enableBox->setChecked(workingEnabled[slotIndex]);
        auto* selectButton = new QPushButton(QStringLiteral("Plugin..."), container);
        auto* parameterButton = new QPushButton(QStringLiteral("Parameters..."), container);
        auto* clearButton = new QPushButton(QStringLiteral("Clear"), container);

        pluginLabels[slotIndex] = pluginLabel;
        enabledBoxes[slotIndex] = enableBox;
        parameterButtons[slotIndex] = parameterButton;

        connect(enableBox, &QCheckBox::toggled, &dialog, [&, slotIndex](bool checked) {
            workingEnabled[slotIndex] = checked;
        });
        connect(selectButton, &QPushButton::clicked, &dialog, [&, slotIndex]() {
            const QString selectedUri = selectLv2PluginUri(&dialog, QStringLiteral("LV2 Slot %1").arg(slotIndex + 1), workingUris[slotIndex]);
            if (selectedUri == workingUris[slotIndex]) {
                return;
            }
            workingUris[slotIndex] = selectedUri;
            workingParameters[slotIndex].clear();
            refreshSlot(slotIndex);
        });
        connect(parameterButton, &QPushButton::clicked, &dialog, [&, slotIndex]() {
            if (workingUris[slotIndex].isEmpty()) {
                return;
            }
            editLv2Parameters(
                workingUris[slotIndex],
                QStringLiteral("LV2 Slot %1 Parameters").arg(slotIndex + 1),
                &workingParameters[slotIndex]);
        });
        connect(clearButton, &QPushButton::clicked, &dialog, [&, slotIndex]() {
            workingUris[slotIndex].clear();
            workingParameters[slotIndex].clear();
            workingEnabled[slotIndex] = true;
            enabledBoxes[slotIndex]->setChecked(true);
            refreshSlot(slotIndex);
        });

        grid->addWidget(slotLabel, static_cast<int>(slotIndex) + 1, 0);
        grid->addWidget(pluginLabel, static_cast<int>(slotIndex) + 1, 1);
        grid->addWidget(enableBox, static_cast<int>(slotIndex) + 1, 2);
        grid->addWidget(selectButton, static_cast<int>(slotIndex) + 1, 3);
        grid->addWidget(parameterButton, static_cast<int>(slotIndex) + 1, 4);
        grid->addWidget(clearButton, static_cast<int>(slotIndex) + 1, 5);
        refreshSlot(slotIndex);
    }

    scrollArea->setWidget(container);
    layout->addWidget(scrollArea);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }

    *uris = workingUris;
    *enabled = workingEnabled;
    *parameterValues = workingParameters;
    return true;
}

void MainWindow::updateBottomStatus()
{
    if (jackSampleRateLabel_ != nullptr) {
        jackSampleRateLabel_->setText(audioEngine_.isConnected()
                ? QStringLiteral("JACK SR: %1 Hz").arg(audioEngine_.audioSampleRate())
                : QStringLiteral("JACK SR: --"));
    }

    if (selectedSampleRateLabel_ == nullptr) {
        return;
    }

    const Sample* sample = samplePoolWidget_ != nullptr ? samplePoolWidget_->selectedSample() : nullptr;
    if (sample == nullptr) {
        selectedSampleRateLabel_->setText(QStringLiteral("Sample SR: --"));
        return;
    }

    const QString nativeState = sample->resampled ? QStringLiteral("resampled") : QStringLiteral("native");
    const QString bpmText = sampleTempoStatusText(sample);
    if (sample->originalSampleRate != 0U && sample->originalSampleRate != sample->sampleRate) {
        selectedSampleRateLabel_->setText(QStringLiteral("Sample SR: %1 -> %2 Hz (%3) | %4")
            .arg(sample->originalSampleRate)
            .arg(sample->sampleRate)
            .arg(nativeState)
            .arg(bpmText));
        return;
    }

    selectedSampleRateLabel_->setText(QStringLiteral("Sample SR: %1 Hz (%2) | %3")
        .arg(sample->originalSampleRate != 0U ? sample->originalSampleRate : sample->sampleRate)
        .arg(nativeState)
        .arg(bpmText));
}

void MainWindow::showKeyContextMenu(int noteIndex, const QPoint& globalPos)
{
    if (noteIndex < 0 || noteIndex >= PianoWidget::kKeyCount) {
        return;
    }

    selectedNoteIndex_ = noteIndex;
    if (mode_ == AppMode::Edit) {
        pianoWidget_->setSelectedNoteIndex(noteIndex);
    }
    updateKeySettingsControls(noteIndex);

    QMenu menu(this);
    QAction* loopAction = menu.addAction(QStringLiteral("Loop"));
    QAction* noLoopAction = menu.addAction(QStringLiteral("No Loop"));
    QAction* virtualStaccatoAction = menu.addAction(QStringLiteral("Virtual Staccato"));
    QAction* selfMixAction = menu.addAction(QStringLiteral("Mix With Itself"));
    loopAction->setCheckable(true);
    noLoopAction->setCheckable(true);
    virtualStaccatoAction->setCheckable(true);
    selfMixAction->setCheckable(true);
    loopAction->setChecked(perKeyLoopMode_[static_cast<size_t>(noteIndex)] == KeyLoopMode::Loop);
    noLoopAction->setChecked(perKeyLoopMode_[static_cast<size_t>(noteIndex)] == KeyLoopMode::NoLoop);
    virtualStaccatoAction->setChecked(perKeyVirtualStaccatoEnabled_[static_cast<size_t>(noteIndex)]);
    selfMixAction->setChecked(perKeySelfMixEnabled_[static_cast<size_t>(noteIndex)]);
    virtualStaccatoAction->setEnabled(perKeyLoopMode_[static_cast<size_t>(noteIndex)] == KeyLoopMode::NoLoop);
    selfMixAction->setEnabled(perKeyLoopMode_[static_cast<size_t>(noteIndex)] == KeyLoopMode::NoLoop);
    menu.addSeparator();
    QAction* attackAction = menu.addAction(QStringLiteral("Attack... (%1 ms)")
        .arg(perKeyAttackMs_[static_cast<size_t>(noteIndex)], 0, 'f', 1));
    QAction* releaseAction = menu.addAction(QStringLiteral("Release... (%1 ms)")
        .arg(perKeyReleaseMs_[static_cast<size_t>(noteIndex)], 0, 'f', 1));
    QAction* staccatoAction = menu.addAction(QStringLiteral("Staccato... (%1%)")
        .arg(perKeyStaccatoPercent_[static_cast<size_t>(noteIndex)]));
    menu.addSeparator();
    int activeLv2Slots = 0;
    for (size_t slotIndex = 0; slotIndex < AudioEngine::kLv2ChainSize; ++slotIndex) {
        if (!perKeyLv2PluginUris_[static_cast<size_t>(noteIndex)][slotIndex].isEmpty()) {
            ++activeLv2Slots;
        }
    }
    QAction* lv2ChainAction = menu.addAction(QStringLiteral("LV2 Chain... (%1/5)")
        .arg(activeLv2Slots));

    QAction* selectedAction = menu.exec(globalPos);
    if (selectedAction == nullptr) {
        return;
    }

    if (selectedAction == loopAction || selectedAction == noLoopAction) {
        perKeyLoopMode_[static_cast<size_t>(noteIndex)] =
            selectedAction == noLoopAction ? KeyLoopMode::NoLoop : KeyLoopMode::Loop;
        if (selectedAction == noLoopAction) {
            perKeyVirtualStaccatoEnabled_[static_cast<size_t>(noteIndex)] = true;
            perKeySelfMixEnabled_[static_cast<size_t>(noteIndex)] = true;
        }
        audioEngine_.setKeyLoopMode(noteIndex, perKeyLoopMode_[static_cast<size_t>(noteIndex)]);
        audioEngine_.setKeyVirtualStaccatoEnabled(noteIndex, perKeyVirtualStaccatoEnabled_[static_cast<size_t>(noteIndex)]);
        audioEngine_.setKeySelfMixEnabled(noteIndex, perKeySelfMixEnabled_[static_cast<size_t>(noteIndex)]);
        updateSelectedKeyControls(noteIndex);
        statusBar()->showMessage(QStringLiteral("%1 loop mode: %2")
            .arg(noteNameForIndex(noteIndex), loopModeName(perKeyLoopMode_[static_cast<size_t>(noteIndex)])));
        return;
    }

    if (selectedAction == virtualStaccatoAction) {
        perKeyVirtualStaccatoEnabled_[static_cast<size_t>(noteIndex)] = !perKeyVirtualStaccatoEnabled_[static_cast<size_t>(noteIndex)];
        audioEngine_.setKeyVirtualStaccatoEnabled(noteIndex, perKeyVirtualStaccatoEnabled_[static_cast<size_t>(noteIndex)]);
        updateSelectedKeyControls(noteIndex);
        statusBar()->showMessage(QStringLiteral("%1 virtual staccato: %2")
            .arg(noteNameForIndex(noteIndex), perKeyVirtualStaccatoEnabled_[static_cast<size_t>(noteIndex)] ? QStringLiteral("On") : QStringLiteral("Off")));
        return;
    }

    if (selectedAction == selfMixAction) {
        perKeySelfMixEnabled_[static_cast<size_t>(noteIndex)] = !perKeySelfMixEnabled_[static_cast<size_t>(noteIndex)];
        audioEngine_.setKeySelfMixEnabled(noteIndex, perKeySelfMixEnabled_[static_cast<size_t>(noteIndex)]);
        updateSelectedKeyControls(noteIndex);
        statusBar()->showMessage(QStringLiteral("%1 self-mix: %2")
            .arg(noteNameForIndex(noteIndex), perKeySelfMixEnabled_[static_cast<size_t>(noteIndex)] ? QStringLiteral("On") : QStringLiteral("Off")));
        return;
    }

    if (selectedAction == attackAction) {
        bool ok = false;
        const double value = QInputDialog::getDouble(
            this,
            QStringLiteral("Key Attack"),
            QStringLiteral("Attack milliseconds:"),
            perKeyAttackMs_[static_cast<size_t>(noteIndex)],
            0.0,
            5000.0,
            1,
            &ok);
        if (!ok) {
            return;
        }
        perKeyAttackMs_[static_cast<size_t>(noteIndex)] = value;
        audioEngine_.setKeyAttackMs(noteIndex, value);
        return;
    }

    if (selectedAction == releaseAction) {
        bool ok = false;
        const double value = QInputDialog::getDouble(
            this,
            QStringLiteral("Key Release"),
            QStringLiteral("Release milliseconds:"),
            perKeyReleaseMs_[static_cast<size_t>(noteIndex)],
            0.0,
            5000.0,
            1,
            &ok);
        if (!ok) {
            return;
        }
        perKeyReleaseMs_[static_cast<size_t>(noteIndex)] = value;
        audioEngine_.setKeyReleaseMs(noteIndex, value);
        return;
    }

    if (selectedAction == staccatoAction) {
        bool ok = false;
        const int value = QInputDialog::getInt(
            this,
            QStringLiteral("Key Staccato"),
            QStringLiteral("Staccato percent:"),
            perKeyStaccatoPercent_[static_cast<size_t>(noteIndex)],
            0,
            100,
            1,
            &ok);
        if (!ok) {
            return;
        }
        perKeyStaccatoPercent_[static_cast<size_t>(noteIndex)] = value;
        audioEngine_.setKeyStaccato(noteIndex, static_cast<float>(value) / 100.0F);
        return;
    }

    if (selectedAction == lv2ChainAction) {
        if (!editLv2Chain(
                QStringLiteral("%1 LV2 Chain").arg(noteNameForIndex(noteIndex)),
                &perKeyLv2PluginUris_[static_cast<size_t>(noteIndex)],
                &perKeyLv2Enabled_[static_cast<size_t>(noteIndex)],
                &perKeyLv2ParameterValues_[static_cast<size_t>(noteIndex)])) {
            return;
        }

        for (size_t slotIndex = 0; slotIndex < AudioEngine::kLv2ChainSize; ++slotIndex) {
            QString errorMessage;
            if (!audioEngine_.setKeyLv2SlotUri(noteIndex, slotIndex, perKeyLv2PluginUris_[static_cast<size_t>(noteIndex)][slotIndex], &errorMessage)) {
                QMessageBox::warning(this, QStringLiteral("Key LV2 Chain"), errorMessage);
                return;
            }
            audioEngine_.setKeyLv2SlotEnabled(noteIndex, slotIndex, perKeyLv2Enabled_[static_cast<size_t>(noteIndex)][slotIndex]);
            audioEngine_.setKeyLv2SlotParameterValues(noteIndex, slotIndex, perKeyLv2ParameterValues_[static_cast<size_t>(noteIndex)][slotIndex]);
        }
        saveSettings();
        statusBar()->showMessage(QStringLiteral("%1 LV2 chain updated.").arg(noteNameForIndex(noteIndex)));
    }
}

void MainWindow::importSfz()
{
    const QString sfzPath = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("Import SFZ"),
        QDir::homePath(),
        QStringLiteral("SFZ instrument (*.sfz)"));
    if (sfzPath.isEmpty()) {
        return;
    }

    QFile file(sfzPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, QStringLiteral("Import SFZ"), QStringLiteral("Could not open %1").arg(sfzPath));
        return;
    }

    const QDir sfzDir = QFileInfo(sfzPath).absoluteDir();
    const QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));

    struct SfzRegion {
        QHash<QString, QString> opcodes;
    };

    QHash<QString, QString> controlOpcodes;
    QHash<QString, QString> globalOpcodes;
    QHash<QString, QString> groupOpcodes;
    std::vector<SfzRegion> regions;
    QString currentSection;

    static const QRegularExpression tagRegex(QStringLiteral("<\\s*([^>]+)\\s*>"));

    for (const QString& rawLine : lines) {
        QString line = stripSfzComments(rawLine).trimmed();
        if (line.isEmpty()) {
            continue;
        }

        const QRegularExpressionMatch tagMatch = tagRegex.match(line);
        const bool hasTag = tagMatch.hasMatch();
        if (hasTag) {
            currentSection = tagMatch.captured(1).trimmed().toLower();
            if (currentSection == QStringLiteral("group")) {
                groupOpcodes.clear();
            } else if (currentSection == QStringLiteral("region")) {
                SfzRegion region;
                region.opcodes = controlOpcodes;
                for (auto it = globalOpcodes.cbegin(); it != globalOpcodes.cend(); ++it) {
                    region.opcodes.insert(it.key(), it.value());
                }
                for (auto it = groupOpcodes.cbegin(); it != groupOpcodes.cend(); ++it) {
                    region.opcodes.insert(it.key(), it.value());
                }
                regions.push_back(std::move(region));
            }
            line = line.mid(tagMatch.capturedEnd()).trimmed();
        }

        if (line.isEmpty() && currentSection != QStringLiteral("region")) {
            continue;
        }

        const QHash<QString, QString> opcodes = parseSfzOpcodes(line);
        if (currentSection == QStringLiteral("control")) {
            for (auto it = opcodes.cbegin(); it != opcodes.cend(); ++it) {
                controlOpcodes.insert(it.key(), it.value());
            }
        } else if (currentSection == QStringLiteral("global")) {
            for (auto it = opcodes.cbegin(); it != opcodes.cend(); ++it) {
                globalOpcodes.insert(it.key(), it.value());
            }
        } else if (currentSection == QStringLiteral("group")) {
            for (auto it = opcodes.cbegin(); it != opcodes.cend(); ++it) {
                groupOpcodes.insert(it.key(), it.value());
            }
        } else if (currentSection == QStringLiteral("region")) {
            if (regions.empty()) {
                SfzRegion region;
                region.opcodes = controlOpcodes;
                for (auto it = globalOpcodes.cbegin(); it != globalOpcodes.cend(); ++it) {
                    region.opcodes.insert(it.key(), it.value());
                }
                for (auto it = groupOpcodes.cbegin(); it != groupOpcodes.cend(); ++it) {
                    region.opcodes.insert(it.key(), it.value());
                }
                regions.push_back(std::move(region));
            }
            for (auto it = opcodes.cbegin(); it != opcodes.cend(); ++it) {
                regions.back().opcodes.insert(it.key(), it.value());
            }
        }
    }

    QHash<QString, Sample*> sampleCache;
    QHash<QString, bool> failedPaths;
    int loadedSamples = 0;
    int failedSamples = 0;
    int skippedRegions = 0;
    int assignedKeys = 0;
    Sample* lastImportedSample = nullptr;

    for (const SfzRegion& region : regions) {
        const QString sampleToken = region.opcodes.value(QStringLiteral("sample"));
        const QString samplePath = resolveSfzPath(
            sfzDir,
            region.opcodes.value(QStringLiteral("default_path")),
            sampleToken);
        if (samplePath.isEmpty()) {
            ++skippedRegions;
            continue;
        }

        Sample* sample = sampleCache.value(samplePath, nullptr);
        if (sample == nullptr) {
            LoadedSample loadedSample = SampleLoader::loadAudioFile(
                samplePath,
                audioEngine_.isConnected() ? audioEngine_.audioSampleRate() : 0U,
                resamplerBackend_,
                resamplerQuality_);
            if (!loadedSample.ok) {
                if (!failedPaths.contains(samplePath)) {
                    failedPaths.insert(samplePath, true);
                    ++failedSamples;
                }
                ++skippedRegions;
                continue;
            }

            sample = samplePoolWidget_->addStateSample(loadedSample);
            if (sample == nullptr) {
                ++skippedRegions;
                continue;
            }
            sampleCache.insert(samplePath, sample);
            ++loadedSamples;
            lastImportedSample = sample;
        }

        int lowMidi = -1;
        int highMidi = -1;
        int keyCenter = -1;
        if (region.opcodes.contains(QStringLiteral("key"))) {
            lowMidi = highMidi = midiNoteFromToken(region.opcodes.value(QStringLiteral("key")));
        } else if (region.opcodes.contains(QStringLiteral("pitch_keycenter"))) {
            lowMidi = highMidi = midiNoteFromToken(region.opcodes.value(QStringLiteral("pitch_keycenter")));
        } else {
            if (region.opcodes.contains(QStringLiteral("lokey"))) {
                lowMidi = midiNoteFromToken(region.opcodes.value(QStringLiteral("lokey")));
            }
            if (region.opcodes.contains(QStringLiteral("hikey"))) {
                highMidi = midiNoteFromToken(region.opcodes.value(QStringLiteral("hikey")));
            }
        }
        if (region.opcodes.contains(QStringLiteral("pitch_keycenter"))) {
            keyCenter = midiNoteFromToken(region.opcodes.value(QStringLiteral("pitch_keycenter")));
        }

        if (lowMidi < 0 && highMidi >= 0) {
            lowMidi = highMidi;
        }
        if (highMidi < 0 && lowMidi >= 0) {
            highMidi = lowMidi;
        }
        if (keyCenter < 0) {
            keyCenter = lowMidi >= 0 ? lowMidi : highMidi;
        }
        if (lowMidi < 0 || highMidi < 0) {
            ++skippedRegions;
            continue;
        }

        const double attackMs = std::clamp(region.opcodes.value(QStringLiteral("ampeg_attack")).toDouble() * 1000.0, 0.0, 5000.0);
        const double releaseMs = std::clamp(region.opcodes.value(QStringLiteral("ampeg_release")).toDouble() * 1000.0, 0.0, 5000.0);
        const KeyLoopMode loopMode = keyLoopModeFromSfz(sfzLoopModeValue(region.opcodes));
        const bool virtualStaccatoEnabled = region.opcodes.contains(QStringLiteral("x_superlooper_virtual_staccato"))
            ? region.opcodes.value(QStringLiteral("x_superlooper_virtual_staccato")).toInt() != 0
            : (loopMode == KeyLoopMode::NoLoop);
        const bool selfMixEnabled = region.opcodes.value(QStringLiteral("x_superlooper_selfmix")).toInt() != 0
            || loopMode == KeyLoopMode::NoLoop;
        const double regionPan = std::clamp(region.opcodes.value(QStringLiteral("pan"), QStringLiteral("0")).toDouble(), -100.0, 100.0);
        const int regionVolumePercent = percentFromDecibels(sfzCombinedGainDecibels(region.opcodes));
        const int transposeSemitones = region.opcodes.value(QStringLiteral("transpose")).toInt(0);
        const double tuneCents = region.opcodes.value(QStringLiteral("tune"), QStringLiteral("0")).toDouble();
        const double pitchKeytrack = sfzPitchKeytrackPercent(region.opcodes) / 100.0;
        const size_t sampleFrames = sample->frames;
        const auto [trimStart, trimEnd] = sfzTrimRangeFromOpcodes(region.opcodes, sampleFrames);

        for (int midiNote = std::max(lowMidi, PianoWidget::kLowestMidiNote);
             midiNote <= std::min(highMidi, PianoWidget::kLowestMidiNote + PianoWidget::kKeyCount - 1);
             ++midiNote) {
            const int noteIndex = midiNote - PianoWidget::kLowestMidiNote;
            perKeyAttackMs_[static_cast<size_t>(noteIndex)] = attackMs;
            perKeyReleaseMs_[static_cast<size_t>(noteIndex)] = releaseMs;
            perKeyStaccatoPercent_[static_cast<size_t>(noteIndex)] = 0;
            perKeyLoopMode_[static_cast<size_t>(noteIndex)] = loopMode;
            perKeyVirtualStaccatoEnabled_[static_cast<size_t>(noteIndex)] = virtualStaccatoEnabled;
            perKeySelfMixEnabled_[static_cast<size_t>(noteIndex)] = selfMixEnabled;
            perKeyTrimStart_[static_cast<size_t>(noteIndex)] = trimStart;
            perKeyTrimEnd_[static_cast<size_t>(noteIndex)] = trimEnd;
            perKeyPanPercent_[static_cast<size_t>(noteIndex)] = static_cast<int>(std::llround(regionPan));
            perKeyVolumePercent_[static_cast<size_t>(noteIndex)] = regionVolumePercent;
            const double cents = (static_cast<double>(midiNote - keyCenter) * 100.0 * pitchKeytrack)
                + (static_cast<double>(transposeSemitones) * 100.0)
                + tuneCents;
            perKeyPlaybackRate_[static_cast<size_t>(noteIndex)] = std::clamp(std::pow(2.0, cents / 1200.0), 0.0625, 16.0);
            assignSampleToKey(noteIndex, sample);
            ++assignedKeys;
        }
    }

    applyMixerSettings();
    if (lastImportedSample != nullptr) {
        updateWaveformView(lastImportedSample);
        updateSampleFlagControls(lastImportedSample);
    }
    updateBottomStatus();

    const QString summary = QStringLiteral("Loaded samples: %1\nAssigned keys: %2\nFailed samples: %3\nSkipped regions: %4")
        .arg(loadedSamples)
        .arg(assignedKeys)
        .arg(failedSamples)
        .arg(skippedRegions);
    if (assignedKeys == 0) {
        QMessageBox::warning(this, QStringLiteral("Import SFZ"), summary);
    } else {
        QMessageBox::information(this, QStringLiteral("Import SFZ"), summary);
    }
}

void MainWindow::exportSfz()
{
    QString sfzPath = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("Export SFZ"),
        QDir::home().filePath(QStringLiteral("superlooper-export.sfz")),
        QStringLiteral("SFZ instrument (*.sfz)"));
    if (sfzPath.isEmpty()) {
        return;
    }
    if (QFileInfo(sfzPath).suffix().isEmpty()) {
        sfzPath += QStringLiteral(".sfz");
    }

    const QFileInfo sfzInfo(sfzPath);
    QDir exportDir = sfzInfo.absoluteDir();
    QDir sampleDir(exportDir.filePath(safeStateFileBaseName(sfzInfo.fileName()) + QStringLiteral("_samples")));
    if (!sampleDir.exists() && !exportDir.mkpath(sampleDir.dirName())) {
        QMessageBox::warning(this, QStringLiteral("Export SFZ"), QStringLiteral("Could not create sample folder %1").arg(sampleDir.absolutePath()));
        return;
    }

    QHash<const Sample*, QString> exportedPaths;
    QStringList lines;
    lines << QStringLiteral("// SuperLooper SFZ export")
          << QStringLiteral("<control>")
          << QStringLiteral("default_path=%1").arg(QDir::cleanPath(sampleDir.dirName()))
          << QString();

    int exportedRegions = 0;
    for (int noteIndex = 0; noteIndex < PianoWidget::kKeyCount; ++noteIndex) {
        const Sample* sample = assignedSamples_[static_cast<size_t>(noteIndex)];
        if (sample == nullptr) {
            continue;
        }

        QString sampleRelativePath = exportedPaths.value(sample);
        if (sampleRelativePath.isEmpty()) {
            QString sourcePath = sample->sourceFilePath;
            if (sourcePath.isEmpty() || !QFileInfo::exists(sourcePath)) {
                const QString wavPath = uniqueWavPath(sampleDir, sample->userLabel.isEmpty() ? sample->name : sample->userLabel);
                QString errorMessage;
                if (!samplePoolWidget_->saveSampleToWav(const_cast<Sample*>(sample), wavPath, &errorMessage)) {
                    QMessageBox::warning(this, QStringLiteral("Export SFZ"), QStringLiteral("Could not write %1: %2").arg(sample->name, errorMessage));
                    return;
                }
                sourcePath = wavPath;
            }

            sampleRelativePath = exportDir.relativeFilePath(sourcePath);
            sampleRelativePath.replace(QDir::separator(), QLatin1Char('/'));
            exportedPaths.insert(sample, sampleRelativePath);
        }

        const int midiNote = PianoWidget::kLowestMidiNote + noteIndex;
        QString sampleValue = sampleRelativePath;
        if (sampleValue.contains(QLatin1Char(' '))) {
            sampleValue = QStringLiteral("\"%1\"").arg(sampleValue);
        }

        QString regionLine = QStringLiteral("<region> sample=%1 key=%2")
            .arg(sampleValue)
            .arg(midiNote);
        if (perKeyTrimStart_[static_cast<size_t>(noteIndex)] > 0.0) {
            const int offsetFrames = static_cast<int>(std::llround(perKeyTrimStart_[static_cast<size_t>(noteIndex)] * static_cast<double>(sample->frames)));
            regionLine += QStringLiteral(" offset=%1").arg(offsetFrames);
        }
        if (perKeyTrimEnd_[static_cast<size_t>(noteIndex)] < 1.0) {
            const int endFrames = static_cast<int>(std::llround(perKeyTrimEnd_[static_cast<size_t>(noteIndex)] * static_cast<double>(sample->frames)));
            regionLine += QStringLiteral(" end=%1").arg(endFrames);
        }
        if (perKeyVolumePercent_[static_cast<size_t>(noteIndex)] != 100) {
            const double decibels = 20.0 * std::log10(std::max(0.0001, static_cast<double>(perKeyVolumePercent_[static_cast<size_t>(noteIndex)]) / 100.0));
            regionLine += QStringLiteral(" volume=%1").arg(decibels, 0, 'f', 3);
        }
        if (perKeyPanPercent_[static_cast<size_t>(noteIndex)] != 0) {
            regionLine += QStringLiteral(" pan=%1").arg(perKeyPanPercent_[static_cast<size_t>(noteIndex)]);
        }
        const double cents = 1200.0 * std::log2(std::max(0.0001, perKeyPlaybackRate_[static_cast<size_t>(noteIndex)]));
        const int transpose = static_cast<int>(std::trunc(cents / 100.0));
        const double tune = cents - (static_cast<double>(transpose) * 100.0);
        if (transpose != 0 || std::abs(tune) > 0.01) {
            regionLine += QStringLiteral(" pitch_keycenter=%1").arg(midiNote);
            if (transpose != 0) {
                regionLine += QStringLiteral(" transpose=%1").arg(transpose);
            }
            if (std::abs(tune) > 0.01) {
                regionLine += QStringLiteral(" tune=%1").arg(tune, 0, 'f', 2);
            }
        }
        if (perKeyAttackMs_[static_cast<size_t>(noteIndex)] > 0.0) {
            regionLine += QStringLiteral(" ampeg_attack=%1").arg(perKeyAttackMs_[static_cast<size_t>(noteIndex)] / 1000.0, 0, 'f', 6);
        }
        if (perKeyReleaseMs_[static_cast<size_t>(noteIndex)] > 0.0) {
            regionLine += QStringLiteral(" ampeg_release=%1").arg(perKeyReleaseMs_[static_cast<size_t>(noteIndex)] / 1000.0, 0, 'f', 6);
        }
        const QString loopMode = perKeyLoopMode_[static_cast<size_t>(noteIndex)] == KeyLoopMode::NoLoop
            ? QStringLiteral("no_loop")
            : QStringLiteral("loop_continuous");
        regionLine += QStringLiteral(" loop_mode=%1 loopmode=%1").arg(loopMode);
        if (perKeyLoopMode_[static_cast<size_t>(noteIndex)] == KeyLoopMode::Loop) {
            const int loopStartFrames = static_cast<int>(std::llround(perKeyTrimStart_[static_cast<size_t>(noteIndex)] * static_cast<double>(sample->frames)));
            const int loopEndFrames = static_cast<int>(std::llround(perKeyTrimEnd_[static_cast<size_t>(noteIndex)] * static_cast<double>(sample->frames)));
            regionLine += QStringLiteral(" loop_start=%1 loop_end=%2").arg(loopStartFrames).arg(loopEndFrames);
        }
        if (perKeyVirtualStaccatoEnabled_[static_cast<size_t>(noteIndex)]) {
            regionLine += QStringLiteral(" x_superlooper_virtual_staccato=1");
        }
        if (perKeySelfMixEnabled_[static_cast<size_t>(noteIndex)]) {
            regionLine += QStringLiteral(" x_superlooper_selfmix=1");
        }
        lines << regionLine;
        ++exportedRegions;
    }

    QFile file(sfzPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        QMessageBox::warning(this, QStringLiteral("Export SFZ"), QStringLiteral("Could not write %1").arg(sfzPath));
        return;
    }

    file.write(lines.join(QLatin1Char('\n')).toUtf8());
    statusBar()->showMessage(QStringLiteral("Exported %1 SFZ region%2 to %3")
        .arg(exportedRegions)
        .arg(exportedRegions == 1 ? QString() : QStringLiteral("s"))
        .arg(sfzPath));
}

bool MainWindow::exportAssignedKeymapAsSfzBundle(
    const QString& exportDirectory,
    const QString& baseName,
    QString* sfzPathOut,
    QString* errorMessage)
{
    QDir exportDir(exportDirectory);
    if (!exportDir.exists() && !QDir().mkpath(exportDirectory)) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Could not create %1").arg(exportDirectory);
        }
        return false;
    }

    const QString safeBaseName = safeStateFileBaseName(baseName);
    QDir sampleDir(exportDir.filePath(safeBaseName + QStringLiteral("_samples")));
    if (!sampleDir.exists() && !exportDir.mkpath(sampleDir.dirName())) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Could not create %1").arg(sampleDir.absolutePath());
        }
        return false;
    }

    const QString sfzPath = exportDir.filePath(safeBaseName + QStringLiteral(".sfz"));
    QHash<const Sample*, QString> exportedPaths;
    QStringList lines;
    lines << QStringLiteral("// SuperLooper generated SFZ bundle")
          << QStringLiteral("// superlooper_loop_duration_sec=%1").arg(loopDurationSpinBox_->value(), 0, 'f', 6)
          << QStringLiteral("// superlooper_bars_per_root=%1").arg(barsPerRootSpinBox_->value())
          << QStringLiteral("// superlooper_global_loop_duration_sec=%1").arg(globalLoopDurationSec_, 0, 'f', 6)
          << QStringLiteral("// superlooper_tempo_bpm=%1").arg(tempoBpm_, 0, 'f', 6)
          << QStringLiteral("// superlooper_recording_method=%1").arg(
                 recordingMethod_ == RecordingMethod::ArmedMiddlePedal
                     ? QStringLiteral("ArmedMiddlePedal")
                     : QStringLiteral("KeyPressesAutomatic"))
          << QStringLiteral("// superlooper_resampler=%1").arg(SampleLoader::resamplerBackendName(resamplerBackend_))
          << QStringLiteral("// superlooper_resampler_quality=%1").arg(SampleLoader::resamplerQualityName(resamplerQuality_))
          << QStringLiteral("// superlooper_sample_normalization=%1").arg(sampleNormalizationEnabled_ ? QStringLiteral("true") : QStringLiteral("false"))
          << QStringLiteral("// superlooper_limiter=%1").arg(limiterEnabled_ ? QStringLiteral("true") : QStringLiteral("false"))
          << QStringLiteral("// superlooper_fade_time_ms=%1").arg(fadeTimeMs_, 0, 'f', 3)
          << QStringLiteral("// superlooper_loop_crossfade=%1").arg(loopCrossfadeEnabled_ ? QStringLiteral("true") : QStringLiteral("false"))
          << QStringLiteral("// superlooper_loop_crossfade_ms=%1").arg(loopCrossfadeMs_, 0, 'f', 3)
          << QStringLiteral("// superlooper_master_gain_percent=%1").arg(masterGainPercent_)
          << QStringLiteral("<control>")
          << QStringLiteral("default_path=%1").arg(QDir::cleanPath(sampleDir.dirName()))
          << QString();

    for (size_t slotIndex = 0; slotIndex < AudioEngine::kLv2ChainSize; ++slotIndex) {
        lines << QStringLiteral("// superlooper_master_lv2_slot_%1_uri=%2 enabled=%3")
                     .arg(slotIndex + 1)
                     .arg(masterLv2PluginUris_[slotIndex].isEmpty() ? QStringLiteral("<none>") : masterLv2PluginUris_[slotIndex])
                     .arg(masterLv2Enabled_[slotIndex] ? QStringLiteral("true") : QStringLiteral("false"));
    }
    lines << QString();

    for (int group = 0; group < 4; ++group) {
        lines << QStringLiteral("// group_%1_gain_percent=%2 group_%1_pan_percent=%3 group_%1_muted=%4 group_%1_solo=%5")
                     .arg(group)
                     .arg(groupGainPercent_[static_cast<size_t>(group)])
                     .arg(groupPanPercent_[static_cast<size_t>(group)])
                     .arg(groupMuted_[static_cast<size_t>(group)] ? QStringLiteral("true") : QStringLiteral("false"))
                     .arg(groupSolo_[static_cast<size_t>(group)] ? QStringLiteral("true") : QStringLiteral("false"));
    }
    lines << QString();

    int exportedRegions = 0;
    for (int noteIndex = 0; noteIndex < PianoWidget::kKeyCount; ++noteIndex) {
        const Sample* sample = assignedSamples_[static_cast<size_t>(noteIndex)];
        if (sample == nullptr) {
            continue;
        }

        QString sampleRelativePath = exportedPaths.value(sample);
        if (sampleRelativePath.isEmpty()) {
            const QString wavPath = uniqueWavPath(sampleDir, sample->userLabel.isEmpty() ? sample->name : sample->userLabel);
            Sample copy = *sample;
            QString writeError;
            if (!samplePoolWidget_->saveSampleToWav(&copy, wavPath, &writeError)) {
                if (errorMessage != nullptr) {
                    *errorMessage = QStringLiteral("Could not write %1: %2").arg(sample->name, writeError);
                }
                return false;
            }

            sampleRelativePath = sampleDir.relativeFilePath(wavPath);
            sampleRelativePath.replace(QDir::separator(), QLatin1Char('/'));
            exportedPaths.insert(sample, sampleRelativePath);
        }

        const int midiNote = PianoWidget::kLowestMidiNote + noteIndex;
        QString sampleValue = sampleRelativePath;
        if (sampleValue.contains(QLatin1Char(' '))) {
            sampleValue = QStringLiteral("\"%1\"").arg(sampleValue);
        }

        lines << QStringLiteral("// key=%1 note=%2 sample_name=%3 user_label=%4 source=%5")
                     .arg(noteIndex)
                     .arg(midiNote)
                     .arg(sample->name)
                     .arg(sample->userLabel.isEmpty() ? QStringLiteral("<none>") : sample->userLabel)
                     .arg(sample->sourceFilePath.isEmpty() ? QStringLiteral("<memory>") : sample->sourceFilePath)
              << QStringLiteral("// key_auto=%1 key_muted=%2 key_solo=%3 key_group=%4 key_staccato_percent=%5 key_virtual_staccato=%6 key_self_mix=%7")
                     .arg(perKeyAutoMode_[static_cast<size_t>(noteIndex)] ? QStringLiteral("true") : QStringLiteral("false"))
                     .arg(perKeyMuted_[static_cast<size_t>(noteIndex)] ? QStringLiteral("true") : QStringLiteral("false"))
                     .arg(perKeySolo_[static_cast<size_t>(noteIndex)] ? QStringLiteral("true") : QStringLiteral("false"))
                     .arg(perKeyGroup_[static_cast<size_t>(noteIndex)])
                     .arg(perKeyStaccatoPercent_[static_cast<size_t>(noteIndex)])
                     .arg(perKeyVirtualStaccatoEnabled_[static_cast<size_t>(noteIndex)] ? QStringLiteral("true") : QStringLiteral("false"))
                     .arg(perKeySelfMixEnabled_[static_cast<size_t>(noteIndex)] ? QStringLiteral("true") : QStringLiteral("false"))
              << QStringLiteral("// sample_original_sample_rate=%1 sample_current_sample_rate=%2 sample_is_root=%3 sample_sync_to_root=%4 sample_gain=%5 sample_normalization_gain=%6")
                     .arg(sample->originalSampleRate)
                     .arg(sample->sampleRate)
                     .arg(sample->isRoot ? QStringLiteral("true") : QStringLiteral("false"))
                     .arg(sample->syncToRoot ? QStringLiteral("true") : QStringLiteral("false"))
                     .arg(sample->gain, 0, 'f', 6)
                     .arg(sample->normalizationGain, 0, 'f', 6);

        QString regionLine = QStringLiteral("<region> sample=%1 key=%2")
            .arg(sampleValue)
            .arg(midiNote);
        if (perKeyTrimStart_[static_cast<size_t>(noteIndex)] > 0.0) {
            const int offsetFrames = static_cast<int>(std::llround(perKeyTrimStart_[static_cast<size_t>(noteIndex)] * static_cast<double>(sample->frames)));
            regionLine += QStringLiteral(" offset=%1").arg(offsetFrames);
        }
        if (perKeyTrimEnd_[static_cast<size_t>(noteIndex)] < 1.0) {
            const int endFrames = static_cast<int>(std::llround(perKeyTrimEnd_[static_cast<size_t>(noteIndex)] * static_cast<double>(sample->frames)));
            regionLine += QStringLiteral(" end=%1").arg(endFrames);
        }
        if (perKeyVolumePercent_[static_cast<size_t>(noteIndex)] != 100) {
            const double decibels = 20.0 * std::log10(std::max(0.0001, static_cast<double>(perKeyVolumePercent_[static_cast<size_t>(noteIndex)]) / 100.0));
            regionLine += QStringLiteral(" volume=%1").arg(decibels, 0, 'f', 3);
        }
        if (perKeyPanPercent_[static_cast<size_t>(noteIndex)] != 0) {
            regionLine += QStringLiteral(" pan=%1").arg(perKeyPanPercent_[static_cast<size_t>(noteIndex)]);
        }
        const double cents = 1200.0 * std::log2(std::max(0.0001, perKeyPlaybackRate_[static_cast<size_t>(noteIndex)]));
        const int transpose = static_cast<int>(std::trunc(cents / 100.0));
        const double tune = cents - (static_cast<double>(transpose) * 100.0);
        if (transpose != 0 || std::abs(tune) > 0.01) {
            regionLine += QStringLiteral(" pitch_keycenter=%1").arg(midiNote);
            if (transpose != 0) {
                regionLine += QStringLiteral(" transpose=%1").arg(transpose);
            }
            if (std::abs(tune) > 0.01) {
                regionLine += QStringLiteral(" tune=%1").arg(tune, 0, 'f', 2);
            }
        }
        if (perKeyAttackMs_[static_cast<size_t>(noteIndex)] > 0.0) {
            regionLine += QStringLiteral(" ampeg_attack=%1").arg(perKeyAttackMs_[static_cast<size_t>(noteIndex)] / 1000.0, 0, 'f', 6);
        }
        if (perKeyReleaseMs_[static_cast<size_t>(noteIndex)] > 0.0) {
            regionLine += QStringLiteral(" ampeg_release=%1").arg(perKeyReleaseMs_[static_cast<size_t>(noteIndex)] / 1000.0, 0, 'f', 6);
        }
        const QString loopMode = perKeyLoopMode_[static_cast<size_t>(noteIndex)] == KeyLoopMode::NoLoop
            ? QStringLiteral("no_loop")
            : QStringLiteral("loop_continuous");
        regionLine += QStringLiteral(" loop_mode=%1 loopmode=%1").arg(loopMode);
        if (perKeyLoopMode_[static_cast<size_t>(noteIndex)] == KeyLoopMode::Loop) {
            const int loopStartFrames = static_cast<int>(std::llround(perKeyTrimStart_[static_cast<size_t>(noteIndex)] * static_cast<double>(sample->frames)));
            const int loopEndFrames = static_cast<int>(std::llround(perKeyTrimEnd_[static_cast<size_t>(noteIndex)] * static_cast<double>(sample->frames)));
            regionLine += QStringLiteral(" loop_start=%1 loop_end=%2").arg(loopStartFrames).arg(loopEndFrames);
        }
        if (perKeyVirtualStaccatoEnabled_[static_cast<size_t>(noteIndex)]) {
            regionLine += QStringLiteral(" x_superlooper_virtual_staccato=1");
        }
        if (perKeySelfMixEnabled_[static_cast<size_t>(noteIndex)]) {
            regionLine += QStringLiteral(" x_superlooper_selfmix=1");
        }
        lines << regionLine;
        lines << QString();
        ++exportedRegions;
    }

    QFile file(sfzPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Could not write %1").arg(sfzPath);
        }
        return false;
    }
    file.write(lines.join(QLatin1Char('\n')).toUtf8());
    if (sfzPathOut != nullptr) {
        *sfzPathOut = sfzPath;
    }
    if (exportedRegions == 0 && errorMessage != nullptr) {
        *errorMessage = QStringLiteral("No assigned keys were available for SFZ export.");
    }
    return exportedRegions > 0;
}

void MainWindow::updatePeakMeter()
{
    if (peakMeter_ == nullptr || limiterLabel_ == nullptr) {
        return;
    }

    const float peak = std::max(audioEngine_.outputPeakLeft(), audioEngine_.outputPeakRight());
    peakMeter_->setValue(std::clamp(static_cast<int>(std::llround(peak * 100.0F)), 0, 100));
    limiterLabel_->setStyleSheet(audioEngine_.limiterWasActive()
            ? QStringLiteral("QLabel { background: #b3261e; color: white; font-weight: 700; }")
            : QStringLiteral("QLabel { background: #2e7d57; color: white; }"));
}

void MainWindow::setRootSample(Sample* sample)
{
    if (sample == nullptr) {
        return;
    }

    for (Sample* poolSample : samplePoolWidget_->allSamples()) {
        poolSample->isRoot = false;
    }

    sample->isRoot = true;
    sample->syncToRoot = false;
    sample->timingSyncState = TimingSyncState::Idle;
    sample->timingError.clear();
    sample->syncedFromTempoBpm = 0.0;
    rootSample_ = sample;
    updateSampleFlagControls(samplePoolWidget_->selectedSample());
    if (sample->timingAnalysisState == TimingAnalysisState::Ready && sample->hasBeatGrid) {
        updateTempoFromRoot();
        syncAllSamplesToRoot();
    } else {
        tempoLabel_->setText(QStringLiteral("BPM: Processing"));
        queueRootTimingAnalysis(sample);
    }
    updateBottomStatus();
    statusBar()->showMessage(QStringLiteral("%1 set as Root").arg(sample->name));
}

void MainWindow::clearRootSample(Sample* sample)
{
    if (sample == nullptr || sample != rootSample_) {
        if (sample != nullptr) {
            sample->isRoot = false;
        }
        updateSampleFlagControls(samplePoolWidget_->selectedSample());
        return;
    }

    sample->isRoot = false;
    rootSample_ = nullptr;
    globalLoopDurationSec_ = 0.0;
    tempoBpm_ = 0.0;
    tempoLabel_->setText(QStringLiteral("Tempo -- BPM"));
    updateSampleFlagControls(samplePoolWidget_->selectedSample());
    updateBottomStatus();
    statusBar()->showMessage(QStringLiteral("Root cleared."));
}

void MainWindow::queueRootTimingAnalysis(Sample* sample)
{
    if (sample == nullptr || sample->frames == 0U || sample->sampleRate == 0U) {
        return;
    }

    auto pendingIt = rootTimingRequestIds_.find(sample);
    if (pendingIt != rootTimingRequestIds_.end() && sample->timingAnalysisState == TimingAnalysisState::Processing) {
        return;
    }

    sample->timingAnalysisState = TimingAnalysisState::Processing;
    sample->timingError.clear();
    sample->currentTempoBpm = 0.0;
    if (sample->syncToRoot && sample != rootSample_) {
        sample->timingSyncState = TimingSyncState::PendingAnalysis;
    }

    const int requestId = nextRootTimingRequestId_++;
    rootTimingRequestIds_[sample] = requestId;

    RootTimingRequest request;
    request.sample = sample;
    request.sampleName = sample->name;
    request.stereoData = sample->data;
    request.sampleRate = sample->sampleRate;
    request.frames = sample->frames;
    request.barsPerRoot = barsPerRootSpinBox_->value();
    request.analysisScope = static_cast<int>(beatDetectionScope_);
    request.mergePolicy = static_cast<int>(beatDetectionMergePolicy_);
    request.analysisStartLengthSeconds = beatDetectionStartLengthSeconds_;
    request.analysisEndLengthSeconds = beatDetectionEndLengthSeconds_;
    request.requestId = requestId;

    QMetaObject::invokeMethod(
        rootTimingWorker_,
        "analyzeRootTiming",
        Qt::QueuedConnection,
        Q_ARG(RootTimingRequest, request));
}

void MainWindow::applyRootTimingTrimToAssignedKeys(
    Sample* sample,
    double previousStart,
    double previousEnd,
    double analyzedStart,
    double analyzedEnd)
{
    if (sample == nullptr) {
        return;
    }

    constexpr double kTrimEpsilon = 0.0005;
    auto approximatelyEqual = [](double left, double right) {
        return std::abs(left - right) <= kTrimEpsilon;
    };

    for (int noteIndex = 0; noteIndex < PianoWidget::kKeyCount; ++noteIndex) {
        if (assignedSamples_[static_cast<size_t>(noteIndex)] != sample) {
            continue;
        }

        const size_t keyIndex = static_cast<size_t>(noteIndex);
        const bool usingDefaultTrim =
            approximatelyEqual(perKeyTrimStart_[keyIndex], 0.0)
            && approximatelyEqual(perKeyTrimEnd_[keyIndex], 1.0);
        const bool followingPreviousSampleTrim =
            approximatelyEqual(perKeyTrimStart_[keyIndex], previousStart)
            && approximatelyEqual(perKeyTrimEnd_[keyIndex], previousEnd);
        if (!usingDefaultTrim && !followingPreviousSampleTrim) {
            continue;
        }

        perKeyTrimStart_[keyIndex] = analyzedStart;
        perKeyTrimEnd_[keyIndex] = analyzedEnd;
        audioEngine_.setKeyTrimRange(noteIndex, analyzedStart, analyzedEnd);
    }

    if (selectedNoteIndex_ >= 0 && selectedNoteIndex_ < PianoWidget::kKeyCount) {
        updateSelectedKeyControls(selectedNoteIndex_);
    }
}

void MainWindow::updateSampleTimingStateForSync(Sample* sample)
{
    if (sample == nullptr || sample == rootSample_) {
        return;
    }

    if (!sample->syncToRoot) {
        sample->timingSyncState = TimingSyncState::Idle;
        sample->syncedFromTempoBpm = 0.0;
        return;
    }

    if (sample->timingAnalysisState == TimingAnalysisState::Failed || (!sample->hasBeatGrid && sample->hasTimingAnalysis)) {
        sample->timingSyncState = TimingSyncState::Failed;
        sample->timingError = sample->timingError.isEmpty()
            ? QStringLiteral("No stable beat grid found for sync.")
            : sample->timingError;
        updateBottomStatus();
        statusBar()->showMessage(QStringLiteral("Could not detect a stable BPM for %1").arg(sample->name));
        return;
    }

    if (sample->timingAnalysisState != TimingAnalysisState::Ready || !sample->hasBeatGrid) {
        sample->timingSyncState = TimingSyncState::PendingAnalysis;
        queueRootTimingAnalysis(sample);
        updateBottomStatus();
        statusBar()->showMessage(QStringLiteral("Detecting BPM for %1").arg(sample->name));
        return;
    }

    if (rootSample_ == nullptr || rootSample_->timingAnalysisState != TimingAnalysisState::Ready || !rootSample_->hasBeatGrid) {
        sample->timingSyncState = TimingSyncState::PendingRoot;
        updateBottomStatus();
        statusBar()->showMessage(QStringLiteral("Waiting for Root BPM before syncing %1").arg(sample->name));
        return;
    }

    sample->syncedFromTempoBpm = sample->detectedTempoBpm;
    queueStretchToRoot(sample);
}

double MainWindow::rootTimingDurationSeconds(const Sample* sample) const
{
    if (sample == nullptr || sample->sampleRate == 0U || sample->frames == 0U) {
        return 0.0;
    }

    if (sample->timingAnalysisState != TimingAnalysisState::Ready || !sample->hasBeatGrid || sample->analyzedBeatPeriodFrames <= 0.0) {
        return 0.0;
    }

    const double startFrame = std::clamp(sample->analyzedRootStartFrame, 0.0, static_cast<double>(sample->frames));
    const double desiredEnd = std::clamp(
        startFrame + (sample->analyzedBeatPeriodFrames * static_cast<double>(std::max(1, barsPerRootSpinBox_->value()) * 4)),
        startFrame,
        static_cast<double>(sample->frames));
    const double analyzedDuration = (desiredEnd - startFrame) / static_cast<double>(sample->sampleRate);
    return analyzedDuration > 0.0 ? analyzedDuration : 0.0;
}

void MainWindow::updateTempoFromRoot()
{
    if (rootSample_ != nullptr && rootSample_->timingAnalysisState == TimingAnalysisState::Processing) {
        globalLoopDurationSec_ = 0.0;
        tempoBpm_ = 0.0;
        tempoLabel_->setText(QStringLiteral("BPM: Processing"));
        return;
    }

    if (rootSample_ != nullptr && rootSample_->timingAnalysisState == TimingAnalysisState::Failed) {
        globalLoopDurationSec_ = 0.0;
        tempoBpm_ = 0.0;
        tempoLabel_->setText(QStringLiteral("BPM: Analysis failed"));
        return;
    }

    const double duration = rootTimingDurationSeconds(rootSample_);
    if (duration <= 0.0) {
        globalLoopDurationSec_ = 0.0;
        tempoBpm_ = 0.0;
        tempoLabel_->setText(rootSample_ != nullptr
                ? QStringLiteral("BPM: Waiting")
                : QStringLiteral("Tempo -- BPM"));
        return;
    }

    globalLoopDurationSec_ = duration;
    tempoBpm_ = (static_cast<double>(barsPerRootSpinBox_->value()) * 240.0) / globalLoopDurationSec_;
    tempoLabel_->setText(QStringLiteral("%1 BPM").arg(tempoBpm_, 0, 'f', 1));
    if (rootSample_ != nullptr) {
        rootSample_->currentTempoBpm = tempoBpm_;
    }
}

void MainWindow::syncAllSamplesToRoot()
{
    if (rootSample_ == nullptr) {
        return;
    }

    for (Sample* sample : samplePoolWidget_->allSamples()) {
        if (sample == rootSample_ || !sample->syncToRoot) {
            continue;
        }

        updateSampleTimingStateForSync(sample);
    }
}

void MainWindow::queueStretchToRoot(Sample* sample)
{
    if (sample == nullptr || rootSample_ == nullptr || tempoBpm_ <= 0.0) {
        return;
    }

    if (sample == rootSample_) {
        return;
    }

    if (sample->timingAnalysisState != TimingAnalysisState::Ready || !sample->hasBeatGrid) {
        sample->timingSyncState = TimingSyncState::PendingAnalysis;
        updateBottomStatus();
        return;
    }

    if (rootSample_->timingAnalysisState != TimingAnalysisState::Ready || !rootSample_->hasBeatGrid) {
        sample->timingSyncState = TimingSyncState::PendingRoot;
        updateBottomStatus();
        return;
    }

    const double sourceTempoBpm = sample->detectedTempoBpm > 0.0 ? sample->detectedTempoBpm : sample->analyzedTempoBpm;
    if (sourceTempoBpm <= 0.0) {
        sample->timingSyncState = TimingSyncState::Failed;
        sample->timingError = QStringLiteral("No detected BPM available for sync.");
        updateBottomStatus();
        return;
    }

    const double currentDuration = sampleDurationSeconds(sample);
    if (currentDuration <= 0.0 || sample->data.size() < sample->frames * 2U) {
        sample->timingSyncState = TimingSyncState::Failed;
        sample->timingError = QStringLiteral("Invalid sample data for sync.");
        updateBottomStatus();
        return;
    }

    const double targetDurationSec = currentDuration * (sourceTempoBpm / tempoBpm_);
    if (targetDurationSec <= 0.0) {
        sample->timingSyncState = TimingSyncState::Failed;
        sample->timingError = QStringLiteral("Invalid target duration for sync.");
        updateBottomStatus();
        return;
    }

    if (std::abs(sourceTempoBpm - tempoBpm_) < 0.05) {
        sample->timingSyncState = TimingSyncState::Synced;
        sample->timingError.clear();
        sample->syncedFromTempoBpm = sourceTempoBpm;
        sample->currentTempoBpm = tempoBpm_;
        updateBottomStatus();
        statusBar()->showMessage(QStringLiteral("%1 already matches Root BPM").arg(sample->name));
        return;
    }

    const int requestId = nextStretchRequestId_++;
    stretchRequestIds_[sample] = requestId;
    sample->timingSyncState = TimingSyncState::Syncing;
    sample->timingError.clear();

    StretchRequest request;
    request.sample = sample;
    request.sampleName = sample->name;
    request.stereoData = sample->data;
    request.sampleRate = sample->sampleRate;
    request.frames = sample->frames;
    request.targetDurationSec = targetDurationSec;
    request.requestId = requestId;

    QMetaObject::invokeMethod(
        rubberBandWorker_,
        "stretchToDuration",
        Qt::QueuedConnection,
        Q_ARG(StretchRequest, request));

    updateBottomStatus();
    statusBar()->showMessage(QStringLiteral("Syncing BPM for %1").arg(sample->name));
}

QString MainWindow::sampleTempoStatusText(const Sample* sample) const
{
    if (sample == nullptr) {
        return QStringLiteral("BPM: --");
    }

    if (sample->timingSyncState == TimingSyncState::Syncing) {
        if (sample->detectedTempoBpm > 0.0 && tempoBpm_ > 0.0) {
            return QStringLiteral("BPM: Syncing %1 > %2")
                .arg(sample->detectedTempoBpm, 0, 'f', 1)
                .arg(tempoBpm_, 0, 'f', 1);
        }
        return QStringLiteral("BPM: Syncing BPM");
    }

    if (sample->timingAnalysisState == TimingAnalysisState::Processing
        || sample->timingSyncState == TimingSyncState::PendingAnalysis
        || sample->timingSyncState == TimingSyncState::PendingRoot) {
        return QStringLiteral("BPM: Processing");
    }

    if (sample->timingSyncState == TimingSyncState::Failed || sample->timingAnalysisState == TimingAnalysisState::Failed) {
        return QStringLiteral("BPM: Analysis failed");
    }

    if (sample->timingSyncState == TimingSyncState::Synced
        && sample->syncedFromTempoBpm > 0.0
        && sample->currentTempoBpm > 0.0) {
        return QStringLiteral("BPM: %1 > %2")
            .arg(sample->syncedFromTempoBpm, 0, 'f', 1)
            .arg(sample->currentTempoBpm, 0, 'f', 1);
    }

    if (sample->detectedTempoBpm > 0.0) {
        return QStringLiteral("BPM: %1").arg(sample->detectedTempoBpm, 0, 'f', 1);
    }

    if (sample->currentTempoBpm > 0.0) {
        return QStringLiteral("BPM: %1").arg(sample->currentTempoBpm, 0, 'f', 1);
    }

    return QStringLiteral("BPM: --");
}

QString MainWindow::modeName(AppMode mode)
{
    switch (mode) {
    case AppMode::Normal:
        return QStringLiteral("Normal");
    case AppMode::Record:
        return QStringLiteral("Record");
    case AppMode::Playback:
        return QStringLiteral("Playback");
    case AppMode::Edit:
        return QStringLiteral("Edit");
    }

    return QStringLiteral("Unknown");
}

QString MainWindow::noteNameForIndex(int noteIndex)
{
    static constexpr std::array<const char*, 12> names {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };

    const int midiNote = PianoWidget::kLowestMidiNote + noteIndex;
    const int octave = (midiNote / 12) - 1;
    return QStringLiteral("%1%2").arg(QString::fromLatin1(names[static_cast<size_t>(midiNote % 12)])).arg(octave);
}

double MainWindow::sampleDurationSeconds(const Sample* sample)
{
    if (sample == nullptr || sample->sampleRate == 0U || sample->frames == 0U) {
        return 0.0;
    }

    return static_cast<double>(sample->frames) / static_cast<double>(sample->sampleRate);
}
