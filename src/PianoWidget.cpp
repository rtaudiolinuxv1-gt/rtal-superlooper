#include "PianoWidget.h"

#include <algorithm>
#include <cmath>

#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QContextMenuEvent>
#include <QDropEvent>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>

namespace {
constexpr const char* kSampleMimeType = "application/x-superlooper-sample";
constexpr const char* kSampleDragSourceMimeType = "application/x-superlooper-sample-source";
constexpr int kWhiteKeyCount = 52;
constexpr int kBlackKeyOffset = 14;
constexpr int kBlackKeyWidthPercent = 62;
constexpr int kBlackKeyHeightPercent = 62;
constexpr int kMinOctaveOffset = -3;
constexpr int kMaxOctaveOffset = 3;

bool hasBlackKeyAfterWhiteDegree(int degree)
{
    return degree == 0 || degree == 2 || degree == 5 || degree == 7 || degree == 9;
}

Sample* samplePointerFromMime(const QByteArray& data)
{
    bool ok = false;
    const quintptr pointerValue = data.toULongLong(&ok);
    if (!ok) {
        return nullptr;
    }

    return reinterpret_cast<Sample*>(pointerValue);
}

std::optional<QPoint> pointFromMime(const QByteArray& data)
{
    const QList<QByteArray> parts = data.split(',');
    if (parts.size() != 2) {
        return std::nullopt;
    }

    bool xOk = false;
    bool yOk = false;
    const int x = parts[0].toInt(&xOk);
    const int y = parts[1].toInt(&yOk);
    if (!xOk || !yOk) {
        return std::nullopt;
    }

    return QPoint(x, y);
}
}

PianoWidget::PianoWidget(QWidget* parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAcceptDrops(true);

    keyToSemitone_ = {
        {Qt::Key_Z, 0},
        {Qt::Key_S, 1},
        {Qt::Key_X, 2},
        {Qt::Key_D, 3},
        {Qt::Key_C, 4},
        {Qt::Key_V, 5},
        {Qt::Key_G, 6},
        {Qt::Key_B, 7},
        {Qt::Key_H, 8},
        {Qt::Key_N, 9},
        {Qt::Key_J, 10},
        {Qt::Key_M, 11},
        {Qt::Key_Q, 12},
        {Qt::Key_2, 13},
        {Qt::Key_W, 14},
        {Qt::Key_3, 15},
        {Qt::Key_E, 16},
        {Qt::Key_R, 17},
        {Qt::Key_5, 18},
        {Qt::Key_T, 19},
        {Qt::Key_6, 20},
        {Qt::Key_Y, 21},
        {Qt::Key_7, 22},
        {Qt::Key_U, 23},
        {Qt::Key_I, 24},
    };
}

QSize PianoWidget::minimumSizeHint() const
{
    return QSize(520, 52);
}

QSize PianoWidget::sizeHint() const
{
    return QSize(1040, 84);
}

int PianoWidget::octaveOffset() const noexcept
{
    return octaveOffset_;
}

void PianoWidget::setSelectedNoteIndex(int noteIndex)
{
    if (noteIndex < 0 || noteIndex >= kKeyCount) {
        selectedNote_.reset();
    } else {
        selectedNote_ = noteIndex;
    }
    update();
}

void PianoWidget::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasFormat(kSampleMimeType) && noteIndexAt(event->pos()).has_value()) {
        updateDragLineFromMime(event->mimeData(), event->pos());
        event->acceptProposedAction();
        return;
    }

    QWidget::dragEnterEvent(event);
}

void PianoWidget::dragLeaveEvent(QDragLeaveEvent* event)
{
    dragLineActive_ = false;
    update();
    emit sampleDragLineChanged(QPoint(), QPoint(), false);
    QWidget::dragLeaveEvent(event);
}

void PianoWidget::dragMoveEvent(QDragMoveEvent* event)
{
    if (event->mimeData()->hasFormat(kSampleMimeType) && noteIndexAt(event->pos()).has_value()) {
        updateDragLineFromMime(event->mimeData(), event->pos());
        event->acceptProposedAction();
        return;
    }

    QWidget::dragMoveEvent(event);
}

void PianoWidget::dropEvent(QDropEvent* event)
{
    if (!event->mimeData()->hasFormat(kSampleMimeType)) {
        QWidget::dropEvent(event);
        return;
    }

    const std::optional<int> noteIndex = noteIndexAt(event->pos());
    Sample* sample = samplePointerFromMime(event->mimeData()->data(kSampleMimeType));
    if (!noteIndex.has_value() || sample == nullptr) {
        QWidget::dropEvent(event);
        return;
    }

    dragLineActive_ = false;
    update();
    emit sampleDragLineChanged(QPoint(), QPoint(), false);
    emit sampleDropped(*noteIndex, sample);
    event->acceptProposedAction();
}

void PianoWidget::contextMenuEvent(QContextMenuEvent* event)
{
    const std::optional<int> noteIndex = noteIndexAt(event->pos());
    if (noteIndex.has_value()) {
        selectedNote_ = *noteIndex;
        update();
        emit keyContextMenuRequested(*noteIndex, event->globalPos());
        event->accept();
        return;
    }

    QWidget::contextMenuEvent(event);
}

void PianoWidget::keyPressEvent(QKeyEvent* event)
{
    if (event->isAutoRepeat()) {
        event->accept();
        return;
    }

    if (event->key() == Qt::Key_BracketLeft) {
        setOctaveOffset(octaveOffset_ - 1);
        event->accept();
        return;
    }

    if (event->key() == Qt::Key_BracketRight) {
        setOctaveOffset(octaveOffset_ + 1);
        event->accept();
        return;
    }

    const int noteIndex = noteIndexForKey(event->key());
    if (noteIndex >= 0) {
        pressNote(noteIndex);
        event->accept();
        return;
    }

    QWidget::keyPressEvent(event);
}

void PianoWidget::keyReleaseEvent(QKeyEvent* event)
{
    if (event->isAutoRepeat()) {
        event->accept();
        return;
    }

    const int noteIndex = noteIndexForKey(event->key());
    if (noteIndex >= 0) {
        releaseNote(noteIndex);
        event->accept();
        return;
    }

    QWidget::keyReleaseEvent(event);
}

void PianoWidget::mousePressEvent(QMouseEvent* event)
{
    setFocus(Qt::MouseFocusReason);

    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    const std::optional<int> noteIndex = noteIndexAt(event->pos());
    if (!noteIndex.has_value()) {
        return;
    }

    mouseNote_ = *noteIndex;
    pressNote(*noteIndex);
}

void PianoWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        releaseMouseNote();
        return;
    }

    QWidget::mouseReleaseEvent(event);
}

void PianoWidget::mouseMoveEvent(QMouseEvent* event)
{
    if ((event->buttons() & Qt::LeftButton) == 0) {
        QWidget::mouseMoveEvent(event);
        return;
    }

    const std::optional<int> noteIndex = noteIndexAt(event->pos());
    if (noteIndex == mouseNote_) {
        return;
    }

    releaseMouseNote();
    if (noteIndex.has_value()) {
        mouseNote_ = *noteIndex;
        pressNote(*noteIndex);
    }
}

void PianoWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);

    const std::array<QRectF, kKeyCount> rects = keyRects();
    const QColor whiteKey(245, 246, 240);
    const QColor whiteSelected(245, 214, 123);
    const QColor whitePressed(154, 210, 225);
    const QColor blackKey(28, 31, 34);
    const QColor blackSelected(173, 128, 34);
    const QColor blackPressed(41, 132, 148);
    const QColor border(57, 63, 68);

    for (int noteIndex = 0; noteIndex < kKeyCount; ++noteIndex) {
        const int midiNote = kLowestMidiNote + noteIndex;
        if (isBlackKey(midiNote)) {
            continue;
        }

        painter.setPen(border);
        const bool pressed = pressedNotes_.contains(noteIndex);
        const bool selected = selectedNote_.has_value() && *selectedNote_ == noteIndex;
        painter.setBrush(pressed ? whitePressed : (selected ? whiteSelected : whiteKey));
        painter.drawRect(rects[static_cast<size_t>(noteIndex)]);
    }

    painter.setPen(QColor(70, 74, 78));
    QFont labelFont = font();
    labelFont.setPointSize(8);
    painter.setFont(labelFont);

    for (int noteIndex = 0; noteIndex < kKeyCount; ++noteIndex) {
        const int midiNote = kLowestMidiNote + noteIndex;
        if (isBlackKey(midiNote)) {
            continue;
        }

        if (midiNote % 12 != 0) {
            continue;
        }

        const QRectF rect = rects[static_cast<size_t>(noteIndex)];
        painter.drawText(rect.adjusted(2.0, 0.0, -2.0, -5.0), Qt::AlignHCenter | Qt::AlignBottom, noteName(midiNote));
    }

    for (int noteIndex = 0; noteIndex < kKeyCount; ++noteIndex) {
        const int midiNote = kLowestMidiNote + noteIndex;
        if (!isBlackKey(midiNote)) {
            continue;
        }

        painter.setPen(QColor(10, 12, 14));
        const bool pressed = pressedNotes_.contains(noteIndex);
        const bool selected = selectedNote_.has_value() && *selectedNote_ == noteIndex;
        painter.setBrush(pressed ? blackPressed : (selected ? blackSelected : blackKey));
        painter.drawRect(rects[static_cast<size_t>(noteIndex)]);
    }

    if (dragLineActive_) {
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(QColor(255, 232, 140), 3, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(dragLineStart_, dragLineEnd_);
        painter.setBrush(QColor(255, 232, 140));
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(dragLineEnd_, 5, 5);
        painter.setRenderHint(QPainter::Antialiasing, false);
    }
}

bool PianoWidget::isBlackKey(int midiNote) noexcept
{
    const int semitone = midiNote % 12;
    return semitone == 1 || semitone == 3 || semitone == 6 || semitone == 8 || semitone == 10;
}

QString PianoWidget::noteName(int midiNote)
{
    static constexpr std::array<const char*, 12> names {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };

    const int octave = (midiNote / 12) - 1;
    return QStringLiteral("%1%2").arg(QString::fromLatin1(names[static_cast<size_t>(midiNote % 12)])).arg(octave);
}

int PianoWidget::noteIndexForKey(int qtKey) const
{
    const auto it = keyToSemitone_.find(qtKey);
    if (it == keyToSemitone_.cend()) {
        return -1;
    }

    const int midiNote = kDefaultBaseMidiNote + (octaveOffset_ * 12) + it->second;
    const int noteIndex = midiNote - kLowestMidiNote;
    if (noteIndex < 0 || noteIndex >= kKeyCount) {
        return -1;
    }

    return noteIndex;
}

std::optional<int> PianoWidget::noteIndexAt(const QPoint& point) const
{
    const std::array<QRectF, kKeyCount> rects = keyRects();

    for (int noteIndex = 0; noteIndex < kKeyCount; ++noteIndex) {
        const int midiNote = kLowestMidiNote + noteIndex;
        if (!isBlackKey(midiNote)) {
            continue;
        }

        if (rects[static_cast<size_t>(noteIndex)].contains(point)) {
            return noteIndex;
        }
    }

    for (int noteIndex = 0; noteIndex < kKeyCount; ++noteIndex) {
        const int midiNote = kLowestMidiNote + noteIndex;
        if (isBlackKey(midiNote)) {
            continue;
        }

        if (rects[static_cast<size_t>(noteIndex)].contains(point)) {
            return noteIndex;
        }
    }

    return std::nullopt;
}

std::array<QRectF, PianoWidget::kKeyCount> PianoWidget::keyRects() const
{
    std::array<QRectF, kKeyCount> rects {};
    const double whiteWidth = static_cast<double>(width()) / static_cast<double>(kWhiteKeyCount);
    const double blackWidth = whiteWidth * static_cast<double>(kBlackKeyWidthPercent) / 100.0;
    const double blackHeight = static_cast<double>(height()) * static_cast<double>(kBlackKeyHeightPercent) / 100.0;

    int whiteIndex = 0;
    double previousWhiteX = 0.0;
    int previousWhiteDegree = -1;

    for (int noteIndex = 0; noteIndex < kKeyCount; ++noteIndex) {
        const int midiNote = kLowestMidiNote + noteIndex;
        const int degree = midiNote % 12;

        if (!isBlackKey(midiNote)) {
            const double x = static_cast<double>(whiteIndex) * whiteWidth;
            rects[static_cast<size_t>(noteIndex)] = QRectF(x, 0.0, std::ceil(whiteWidth), height());
            previousWhiteX = x;
            previousWhiteDegree = degree;
            ++whiteIndex;
            continue;
        }

        double x = previousWhiteX + whiteWidth - (blackWidth / 2.0);
        if (!hasBlackKeyAfterWhiteDegree(previousWhiteDegree)) {
            x -= whiteWidth * static_cast<double>(kBlackKeyOffset) / 100.0;
        }

        rects[static_cast<size_t>(noteIndex)] = QRectF(x, 0.0, blackWidth, blackHeight);
    }

    return rects;
}

void PianoWidget::pressNote(int noteIndex)
{
    if (noteIndex < 0 || noteIndex >= kKeyCount) {
        return;
    }

    if (!pressedNotes_.insert(noteIndex).second) {
        return;
    }

    emit notePressed(noteIndex);
    update();
}

void PianoWidget::releaseNote(int noteIndex)
{
    if (pressedNotes_.erase(noteIndex) == 0) {
        return;
    }

    emit noteReleased(noteIndex);
    update();
}

void PianoWidget::releaseMouseNote()
{
    if (!mouseNote_.has_value()) {
        return;
    }

    const int noteIndex = *mouseNote_;
    mouseNote_.reset();
    releaseNote(noteIndex);
}

void PianoWidget::setOctaveOffset(int octaveOffset)
{
    const int clampedOffset = std::clamp(octaveOffset, kMinOctaveOffset, kMaxOctaveOffset);
    if (clampedOffset == octaveOffset_) {
        return;
    }

    octaveOffset_ = clampedOffset;
    emit octaveChanged(octaveOffset_);
}

void PianoWidget::updateDragLineFromMime(const QMimeData* mimeData, const QPoint& endPoint)
{
    dragLineEnd_ = endPoint;
    dragLineStart_ = QPoint(width() / 2, 0);
    if (mimeData != nullptr && mimeData->hasFormat(kSampleDragSourceMimeType)) {
        const std::optional<QPoint> globalStart = pointFromMime(mimeData->data(kSampleDragSourceMimeType));
        if (globalStart.has_value()) {
            dragLineStart_ = mapFromGlobal(*globalStart);
        }
    }

    dragLineActive_ = true;
    emit sampleDragLineChanged(mapToGlobal(dragLineStart_), mapToGlobal(dragLineEnd_), true);
    update();
}
