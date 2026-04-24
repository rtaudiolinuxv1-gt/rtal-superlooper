#pragma once

#include <array>

#include <QWidget>

/**
 * Three clickable pedals used by the UI. The left pedal toggles app mode.
 */
class PedalBar final : public QWidget
{
    Q_OBJECT

public:
    enum class Pedal {
        Left = 0,
        Middle = 1,
        Right = 2,
    };

    explicit PedalBar(QWidget* parent = nullptr);

    [[nodiscard]] QSize minimumSizeHint() const override;
    [[nodiscard]] QSize sizeHint() const override;

signals:
    void pedalPressed(Pedal pedal);
    void pedalReleased(Pedal pedal);
    void leftPedalActivated();

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    [[nodiscard]] int pedalAt(const QPoint& point) const;
    [[nodiscard]] QRect pedalRect(int index) const;

    std::array<bool, 3> pressed_ {};
};
