#include "PedalBar.h"

#include <QMouseEvent>
#include <QPainter>

PedalBar::PedalBar(QWidget* parent)
    : QWidget(parent)
{
    setMinimumHeight(44);
}

QSize PedalBar::minimumSizeHint() const
{
    return QSize(360, 44);
}

QSize PedalBar::sizeHint() const
{
    return QSize(720, 54);
}

void PedalBar::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    const int index = pedalAt(event->pos());
    if (index < 0) {
        return;
    }

    pressed_[static_cast<size_t>(index)] = true;
    emit pedalPressed(static_cast<Pedal>(index));
    if (index == static_cast<int>(Pedal::Left)) {
        emit leftPedalActivated();
    }
    update();
}

void PedalBar::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }

    for (int index = 0; index < static_cast<int>(pressed_.size()); ++index) {
        if (!pressed_[static_cast<size_t>(index)]) {
            continue;
        }

        pressed_[static_cast<size_t>(index)] = false;
        emit pedalReleased(static_cast<Pedal>(index));
    }

    update();
}

void PedalBar::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    painter.fillRect(rect(), QColor(31, 34, 37));

    static constexpr std::array<const char*, 3> labels { "Left", "Middle", "Right" };
    for (int index = 0; index < static_cast<int>(pressed_.size()); ++index) {
        const QRect pedal = pedalRect(index);
        const bool isPressed = pressed_[static_cast<size_t>(index)];
        const QColor fill = isPressed ? QColor(80, 145, 162) : QColor(70, 75, 81);
        const QColor rim = isPressed ? QColor(162, 218, 229) : QColor(130, 136, 142);

        painter.setPen(QPen(rim, 2));
        painter.setBrush(fill);
        painter.drawRoundedRect(pedal, 6.0, 6.0);

        painter.setPen(Qt::white);
        painter.drawText(pedal, Qt::AlignCenter, QString::fromLatin1(labels[static_cast<size_t>(index)]));
    }
}

int PedalBar::pedalAt(const QPoint& point) const
{
    for (int index = 0; index < static_cast<int>(pressed_.size()); ++index) {
        if (pedalRect(index).contains(point)) {
            return index;
        }
    }

    return -1;
}

QRect PedalBar::pedalRect(int index) const
{
    const int gap = 16;
    const int availableWidth = width() - (gap * 4);
    const int pedalWidth = availableWidth / 3;
    const int pedalHeight = qMax(28, height() - 16);
    const int x = gap + (index * (pedalWidth + gap));
    const int y = (height() - pedalHeight) / 2;
    return QRect(x, y, pedalWidth, pedalHeight);
}
