#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <QObject>
#include <QString>

#include "Sample.h"

struct StretchRequest {
    Sample* sample = nullptr;
    QString sampleName;
    std::vector<float> stereoData;
    uint32_t sampleRate = 48000;
    size_t frames = 0;
    double targetDurationSec = 0.0;
    int requestId = 0;
};

struct StretchResult {
    bool ok = false;
    QString errorMessage;
    Sample* sample = nullptr;
    QString sampleName;
    std::vector<float> stereoData;
    uint32_t sampleRate = 48000;
    size_t frames = 0;
    double targetDurationSec = 0.0;
    int requestId = 0;
};

Q_DECLARE_METATYPE(StretchRequest)
Q_DECLARE_METATYPE(StretchResult)

/**
 * RubberBand time-stretch worker. Lives on a QThread and never touches JACK.
 */
class RubberBandWorker final : public QObject
{
    Q_OBJECT

public:
    explicit RubberBandWorker(QObject* parent = nullptr);

public slots:
    void stretchToDuration(const StretchRequest& request);

signals:
    void stretchFinished(const StretchResult& result);
};
