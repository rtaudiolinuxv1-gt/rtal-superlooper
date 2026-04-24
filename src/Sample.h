#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <QColor>
#include <QPixmap>
#include <QString>

enum class KeyLoopMode : int {
    Loop = 0,
    NoLoop = 1,
};

enum class TimingAnalysisState : int {
    Unanalyzed = 0,
    Processing = 1,
    Ready = 2,
    Failed = 3,
};

enum class TimingSyncState : int {
    Idle = 0,
    PendingAnalysis = 1,
    PendingRoot = 2,
    Syncing = 3,
    Synced = 4,
    Failed = 5,
};

struct KeyArticulation {
    double attackMs = 0.0;
    double releaseMs = 0.0;
    double staccatoPercent = 0.0;
    KeyLoopMode loopMode = KeyLoopMode::Loop;
};

/**
 * GUI-owned sample data. Phase 2 normalizes imported files to stereo float
 * frames so the Phase 3 JACK callback can consume one interleaved layout.
 */
struct Sample {
    std::vector<float> data;
    uint32_t sampleRate = 48000;
    size_t frames = 0;
    double startFrame = 0.0;
    double endFrame = 1.0;
    QString name;
    QString userLabel;
    QString sourceFilePath;
    QString resamplerName;
    QColor color;
    QPixmap thumbnail;
    bool isRoot = false;
    bool syncToRoot = false;
    bool createdInApp = false;
    bool resampled = false;
    float gain = 1.0F;
    float normalizationGain = 1.0F;
    float peakLevel = 0.0F;
    double originalDurationSec = 0.0;
    uint32_t originalSampleRate = 48000;
    TimingAnalysisState timingAnalysisState = TimingAnalysisState::Unanalyzed;
    TimingSyncState timingSyncState = TimingSyncState::Idle;
    double detectedTempoBpm = 0.0;
    double syncedFromTempoBpm = 0.0;
    double currentTempoBpm = 0.0;
    QString timingError;
    bool hasTimingAnalysis = false;
    bool hasBeatGrid = false;
    double analyzedRootStartFrame = 0.0;
    double analyzedActiveEndFrame = 0.0;
    double analyzedBeatPeriodFrames = 0.0;
    double analyzedTempoBpm = 0.0;
    double analyzedTempoConfidence = 0.0;
    int analyzedBeatCount = 0;
    int analyzedOnsetCount = 0;
    char label = '\0';
};

struct LoopState {
    Sample* sample = nullptr;
    bool isPlaying = false;
    bool stopQueued = false;
    size_t playbackFrame = 0;
    bool isRecording = false;
    std::vector<float> recordBuffer;
    size_t recordTargetFrames = 0;
    bool autoMode = false;
};
