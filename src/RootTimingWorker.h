#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <QObject>
#include <QString>

#include "Sample.h"

struct RootTimingRequest {
    Sample* sample = nullptr;
    QString sampleName;
    std::vector<float> stereoData;
    uint32_t sampleRate = 48000;
    size_t frames = 0;
    int barsPerRoot = 1;
    int analysisScope = 0;
    int mergePolicy = 0;
    double analysisStartLengthSeconds = 20.0;
    double analysisEndLengthSeconds = 20.0;
    int requestId = 0;
};

struct RootTimingResult {
    bool ok = false;
    QString errorMessage;
    QString message;
    Sample* sample = nullptr;
    QString sampleName;
    uint32_t sampleRate = 48000;
    size_t frames = 0;
    int barsPerRoot = 1;
    int requestId = 0;
    bool usedAubio = false;
    bool hasBeatGrid = false;
    double suggestedStartFrame = 0.0;
    double suggestedEndFrame = 0.0;
    double beatPeriodFrames = 0.0;
    double tempoBpm = 0.0;
    double confidence = 0.0;
    int beatCount = 0;
    int onsetCount = 0;
};

Q_DECLARE_METATYPE(RootTimingRequest)
Q_DECLARE_METATYPE(RootTimingResult)

class RootTimingWorker final : public QObject
{
    Q_OBJECT

public:
    explicit RootTimingWorker(QObject* parent = nullptr);

public slots:
    void analyzeRootTiming(const RootTimingRequest& request);

signals:
    void analysisFinished(const RootTimingResult& result);
};
