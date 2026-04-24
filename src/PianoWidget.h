#pragma once

#include <array>
#include <map>
#include <optional>
#include <set>

#include <QRectF>
#include <QPoint>
#include <QWidget>

class QDragEnterEvent;
class QDragLeaveEvent;
class QDragMoveEvent;
class QDropEvent;
class QContextMenuEvent;
class QMimeData;
struct Sample;

/**
 * Painted 88-key piano widget with mouse and computer-keyboard note input.
 */
class PianoWidget final : public QWidget
{
    Q_OBJECT

public:
    static constexpr int kKeyCount = 88;
    static constexpr int kLowestMidiNote = 21;
    static constexpr int kDefaultBaseMidiNote = 48;

    explicit PianoWidget(QWidget* parent = nullptr);

    [[nodiscard]] QSize minimumSizeHint() const override;
    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] int octaveOffset() const noexcept;
    void setSelectedNoteIndex(int noteIndex);

signals:
    void notePressed(int noteIndex);
    void noteReleased(int noteIndex);
    void octaveChanged(int octaveOffset);
    void sampleDropped(int noteIndex, Sample* sample);
    void sampleDragLineChanged(const QPoint& globalStart, const QPoint& globalEnd, bool active);
    void keyContextMenuRequested(int noteIndex, const QPoint& globalPos);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    [[nodiscard]] static bool isBlackKey(int midiNote) noexcept;
    [[nodiscard]] static QString noteName(int midiNote);
    [[nodiscard]] int noteIndexForKey(int qtKey) const;
    [[nodiscard]] std::optional<int> noteIndexAt(const QPoint& point) const;
    [[nodiscard]] std::array<QRectF, kKeyCount> keyRects() const;

    void pressNote(int noteIndex);
    void releaseNote(int noteIndex);
    void releaseMouseNote();
    void setOctaveOffset(int octaveOffset);
    void updateDragLineFromMime(const QMimeData* mimeData, const QPoint& endPoint);

    std::map<int, int> keyToSemitone_;
    std::set<int> pressedNotes_;
    std::optional<int> mouseNote_;
    std::optional<int> selectedNote_;
    QPoint dragLineStart_;
    QPoint dragLineEnd_;
    bool dragLineActive_ = false;
    int octaveOffset_ = 0;
};
