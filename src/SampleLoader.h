#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <QColor>
#include <QObject>
#include <QString>
#include <QStringList>

enum class ResamplerBackend : int {
    Automatic = 0,
    Soxr = 1,
    Ffmpeg = 2,
    Libsamplerate = 3,
    RubberBand = 4,
};

enum class ResamplerQuality : int {
    Automatic = 0,
    Low = 1,
    Medium = 2,
    High = 3,
    VeryHigh = 4,
};

/**
 * Plain loaded sample payload passed from the loader thread to the GUI thread.
 */
struct LoadedSample {
    bool ok = false;
    QString errorMessage;
    QString name;
    QString sourceFilePath;
    QColor color;
    std::vector<float> stereoData;
    uint32_t sampleRate = 48000;
    size_t frames = 0;
    uint32_t originalSampleRate = 48000;
    float normalizationGain = 1.0F;
    float peakLevel = 0.0F;
    QString resamplerName;
    QString resamplerQualityName;
    bool resampled = false;
};

Q_DECLARE_METATYPE(LoadedSample)

/**
 * libsndfile-backed file loader. This object lives on a QThread.
 */
class SampleLoader final : public QObject
{
    Q_OBJECT

public:
    explicit SampleLoader(QObject* parent = nullptr);
    [[nodiscard]] static LoadedSample loadAudioFile(
        const QString& filePath,
        uint32_t targetSampleRate = 0,
        ResamplerBackend backend = ResamplerBackend::Automatic,
        ResamplerQuality quality = ResamplerQuality::Automatic);
    [[nodiscard]] static bool resampleStereo(
        std::vector<float>* stereoData,
        size_t* frames,
        uint32_t* sampleRate,
        uint32_t targetSampleRate,
        ResamplerBackend backend,
        ResamplerQuality quality,
        QString* errorMessage = nullptr,
        QString* backendName = nullptr,
        QString* qualityName = nullptr);
    [[nodiscard]] static QStringList availableResamplerBackendNames();
    [[nodiscard]] static QStringList availableResamplerQualityNames(ResamplerBackend backend);
    [[nodiscard]] static QString resamplerBackendName(ResamplerBackend backend);
    [[nodiscard]] static ResamplerBackend resamplerBackendFromName(const QString& name);
    [[nodiscard]] static QString resamplerQualityName(ResamplerQuality quality);
    [[nodiscard]] static ResamplerQuality resamplerQualityFromName(const QString& name);
    [[nodiscard]] static ResamplerQuality bestQualityForBackend(ResamplerBackend backend);

public slots:
    void loadFile(const QString& filePath);
    void setTargetSampleRate(uint32_t sampleRate) noexcept;
    void setResamplerBackend(int backend) noexcept;
    void setResamplerQuality(int quality) noexcept;

private:
    uint32_t targetSampleRate_ = 0;
    ResamplerBackend resamplerBackend_ = ResamplerBackend::Automatic;
    ResamplerQuality resamplerQuality_ = ResamplerQuality::Automatic;

signals:
    void sampleLoaded(const LoadedSample& sample);
};
