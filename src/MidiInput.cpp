#include "MidiInput.h"

#include <exception>
#include <vector>

#include <QMetaObject>
#include <QStringList>

#include <RtMidi.h>

namespace {
constexpr unsigned char kStatusMask = 0xF0;
constexpr unsigned char kNoteOff = 0x80;
constexpr unsigned char kNoteOn = 0x90;
constexpr unsigned char kControlChange = 0xB0;
constexpr unsigned char kSostenutoPedalController = 66;
constexpr unsigned char kSoftPedalController = 67;
constexpr unsigned char kPedalThreshold = 64;
}

MidiInput::MidiInput(QObject* parent)
    : QObject(parent)
{
}

MidiInput::~MidiInput()
{
    disconnectInput();
}

bool MidiInput::connectFirstInput(QString* errorMessage)
{
    return connectInputPort(0U, errorMessage);
}

bool MidiInput::connectInputPort(unsigned int portIndex, QString* errorMessage)
{
    if (midiIn_) {
        disconnectInput();
    }

    try {
        auto midiIn = std::make_unique<RtMidiIn>();
        const unsigned int portCount = midiIn->getPortCount();
        if (portCount == 0U) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("No MIDI input ports are available.");
            }
            return false;
        }

        if (portIndex >= portCount) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("Selected MIDI input port is no longer available.");
            }
            return false;
        }

        midiIn->ignoreTypes(true, false, true);
        midiIn->setCallback(&MidiInput::midiCallback, this);
        midiIn->openPort(portIndex, "SuperLooper MIDI Input");
        midiIn_ = std::move(midiIn);
        softPedalDown_.store(false, std::memory_order_release);
        sostenutoPedalDown_.store(false, std::memory_order_release);
        return true;
    } catch (const RtMidiError& error) {
        if (errorMessage != nullptr) {
            *errorMessage = QString::fromStdString(error.getMessage());
        }
    } catch (const std::exception& error) {
        if (errorMessage != nullptr) {
            *errorMessage = QString::fromLocal8Bit(error.what());
        }
    }

    return false;
}

void MidiInput::disconnectInput()
{
    if (!midiIn_) {
        return;
    }

    midiIn_->cancelCallback();
    midiIn_->closePort();
    midiIn_.reset();
    softPedalDown_.store(false, std::memory_order_release);
    sostenutoPedalDown_.store(false, std::memory_order_release);
}

bool MidiInput::isConnected() const noexcept
{
    return midiIn_ != nullptr;
}

QStringList MidiInput::inputPortNames(QString* errorMessage)
{
    try {
        RtMidiIn midiIn;
        const unsigned int portCount = midiIn.getPortCount();
        QStringList result;
        for (unsigned int portIndex = 0; portIndex < portCount; ++portIndex) {
            result.push_back(QString::fromStdString(midiIn.getPortName(portIndex)));
        }
        return result;
    } catch (const RtMidiError& error) {
        if (errorMessage != nullptr) {
            *errorMessage = QString::fromStdString(error.getMessage());
        }
    } catch (const std::exception& error) {
        if (errorMessage != nullptr) {
            *errorMessage = QString::fromLocal8Bit(error.what());
        }
    }

    return {};
}

void MidiInput::midiCallback(double deltaSeconds, std::vector<unsigned char>* message, void* userData)
{
    Q_UNUSED(deltaSeconds)

    auto* input = static_cast<MidiInput*>(userData);
    if (input == nullptr || message == nullptr) {
        return;
    }

    input->handleMessage(*message);
}

void MidiInput::handleMessage(const std::vector<unsigned char>& message)
{
    if (message.size() < 3U) {
        return;
    }

    const unsigned char status = message[0] & kStatusMask;
    const unsigned char data1 = message[1];
    const unsigned char data2 = message[2];

    if (status == kNoteOn && data2 > 0U) {
        QMetaObject::invokeMethod(this, [this, note = static_cast<int>(data1)] {
            emit midiNoteOn(note);
        }, Qt::QueuedConnection);
        return;
    }

    if (status == kNoteOff || (status == kNoteOn && data2 == 0U)) {
        QMetaObject::invokeMethod(this, [this, note = static_cast<int>(data1)] {
            emit midiNoteOff(note);
        }, Qt::QueuedConnection);
        return;
    }

    if (status == kControlChange && data1 == kSoftPedalController) {
        const bool down = data2 >= kPedalThreshold;
        const bool wasDown = softPedalDown_.exchange(down, std::memory_order_acq_rel);
        if (down && !wasDown) {
            QMetaObject::invokeMethod(this, [this] {
                emit leftPedalActivated();
            }, Qt::QueuedConnection);
        }
        return;
    }

    if (status == kControlChange && data1 == kSostenutoPedalController) {
        const bool down = data2 >= kPedalThreshold;
        const bool wasDown = sostenutoPedalDown_.exchange(down, std::memory_order_acq_rel);
        if (down && !wasDown) {
            QMetaObject::invokeMethod(this, [this] {
                emit middlePedalPressed();
            }, Qt::QueuedConnection);
        } else if (!down && wasDown) {
            QMetaObject::invokeMethod(this, [this] {
                emit middlePedalReleased();
            }, Qt::QueuedConnection);
        }
    }
}
