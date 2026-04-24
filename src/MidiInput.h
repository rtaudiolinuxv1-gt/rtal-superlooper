#pragma once

#include <atomic>
#include <memory>

#include <QObject>
#include <QString>
#include <QStringList>

class RtMidiIn;

/**
 * Optional RtMidi input wrapper. It is inactive until the user connects it.
 */
class MidiInput final : public QObject
{
    Q_OBJECT

public:
    explicit MidiInput(QObject* parent = nullptr);
    ~MidiInput() override;

    MidiInput(const MidiInput&) = delete;
    MidiInput& operator=(const MidiInput&) = delete;

    bool connectFirstInput(QString* errorMessage = nullptr);
    bool connectInputPort(unsigned int portIndex, QString* errorMessage = nullptr);
    void disconnectInput();
    [[nodiscard]] bool isConnected() const noexcept;
    [[nodiscard]] static QStringList inputPortNames(QString* errorMessage = nullptr);

signals:
    void midiNoteOn(int midiNote);
    void midiNoteOff(int midiNote);
    void leftPedalActivated();
    void middlePedalPressed();
    void middlePedalReleased();

private:
    static void midiCallback(double deltaSeconds, std::vector<unsigned char>* message, void* userData);
    void handleMessage(const std::vector<unsigned char>& message);

    std::unique_ptr<RtMidiIn> midiIn_;
    std::atomic_bool softPedalDown_ { false };
    std::atomic_bool sostenutoPedalDown_ { false };
};
