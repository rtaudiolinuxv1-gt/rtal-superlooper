#include "SampleLoader.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <memory>

#include <QFileInfo>

#if SUPERLOOPER_HAS_SOXR
#include <soxr.h>
#endif

#if SUPERLOOPER_HAS_LIBSAMPLERATE
#include <samplerate.h>
#endif

#include <rubberband/RubberBandStretcher.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <sndfile.h>

namespace {
struct FormatContextDeleter {
    void operator()(AVFormatContext* context) const
    {
        if (context != nullptr) {
            avformat_close_input(&context);
        }
    }
};

struct CodecContextDeleter {
    void operator()(AVCodecContext* context) const
    {
        avcodec_free_context(&context);
    }
};

struct PacketDeleter {
    void operator()(AVPacket* packet) const
    {
        av_packet_free(&packet);
    }
};

struct FrameDeleter {
    void operator()(AVFrame* frame) const
    {
        av_frame_free(&frame);
    }
};

struct SwrContextDeleter {
    void operator()(SwrContext* context) const
    {
        swr_free(&context);
    }
};

using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextDeleter>;
using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using SwrContextPtr = std::unique_ptr<SwrContext, SwrContextDeleter>;

ResamplerQuality effectiveQuality(ResamplerBackend backend, ResamplerQuality quality)
{
    if (backend == ResamplerBackend::Automatic) {
        return ResamplerQuality::VeryHigh;
    }
    if (quality == ResamplerQuality::Automatic) {
        return SampleLoader::bestQualityForBackend(backend);
    }
    return quality;
}

QColor colorForAudio(const QString& name, const std::vector<float>& stereoData, size_t frames, uint32_t sampleRate)
{
    uint hash = 2166136261U;
    for (const QChar ch : name) {
        hash ^= static_cast<uint>(ch.unicode());
        hash *= 16777619U;
    }

    if (frames < 3U || stereoData.size() < frames * 2U || sampleRate == 0U) {
        return QColor::fromHsv(static_cast<int>(hash % 360U), 145, 210);
    }

    size_t zeroCrossings = 0;
    size_t transientCount = 0;
    float previous = (stereoData[0] + stereoData[1]) * 0.5F;
    float previousEnvelope = std::abs(previous);

    const size_t stride = std::max<size_t>(1U, frames / 16000U);
    for (size_t frame = stride; frame < frames; frame += stride) {
        const size_t offset = frame * 2U;
        const float mono = (stereoData[offset] + stereoData[offset + 1U]) * 0.5F;
        if ((mono >= 0.0F && previous < 0.0F) || (mono < 0.0F && previous >= 0.0F)) {
            ++zeroCrossings;
        }

        const float envelope = std::abs(mono);
        if (envelope > previousEnvelope * 2.5F && envelope > 0.08F) {
            ++transientCount;
        }

        previous = mono;
        previousEnvelope = (previousEnvelope * 0.96F) + (envelope * 0.04F);
    }

    const double inspectedSamples = static_cast<double>((frames - 1U) / stride);
    const double estimatedFrequency = inspectedSamples <= 0.0
        ? 0.0
        : (static_cast<double>(zeroCrossings) * 0.5 * static_cast<double>(sampleRate)) / (inspectedSamples * static_cast<double>(stride));
    const double pitchClass = estimatedFrequency <= 0.0
        ? static_cast<double>(hash % 12U)
        : std::fmod(12.0 * std::log2(estimatedFrequency / 440.0) + 69.0, 12.0);
    const int hue = static_cast<int>(std::round(std::fmod((pitchClass + 12.0), 12.0) * 30.0)) % 360;
    const int saturation = std::clamp(125 + static_cast<int>(transientCount * 4U), 115, 225);
    return QColor::fromHsv(hue, saturation, 215);
}

void analyzeGain(LoadedSample* sample)
{
    if (sample == nullptr || sample->stereoData.empty()) {
        return;
    }

    float peak = 0.0F;
    double sumSquares = 0.0;
    for (float value : sample->stereoData) {
        const float absValue = std::abs(value);
        peak = std::max(peak, absValue);
        sumSquares += static_cast<double>(value) * static_cast<double>(value);
    }

    const double rms = std::sqrt(sumSquares / static_cast<double>(sample->stereoData.size()));
    constexpr float kTargetPeak = 0.85F;
    constexpr double kTargetRms = 0.18;
    float suggested = 1.0F;
    if (peak > 0.000001F && rms > 0.000001) {
        suggested = static_cast<float>(std::min(
            static_cast<double>(kTargetPeak / peak),
            kTargetRms / rms));
    }

    sample->peakLevel = peak;
    sample->normalizationGain = std::clamp(suggested, 0.10F, 4.0F);
}

bool resampleWithFfmpeg(
    std::vector<float>* stereoData,
    size_t* frames,
    uint32_t sourceRate,
    uint32_t targetRate,
    ResamplerQuality quality,
    QString* errorMessage)
{
    if (stereoData == nullptr || frames == nullptr || sourceRate == 0U || targetRate == 0U) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Invalid resampling input.");
        }
        return false;
    }

    SwrContextPtr swrContext(swr_alloc_set_opts(
        nullptr,
        AV_CH_LAYOUT_STEREO,
        AV_SAMPLE_FMT_FLT,
        static_cast<int>(targetRate),
        AV_CH_LAYOUT_STEREO,
        AV_SAMPLE_FMT_FLT,
        static_cast<int>(sourceRate),
        0,
        nullptr));
    if (!swrContext || swr_init(swrContext.get()) < 0) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Could not initialize FFmpeg resampler.");
        }
        return false;
    }

    const ResamplerQuality selectedQuality = effectiveQuality(ResamplerBackend::Ffmpeg, quality);
    int filterSize = 16;
    int phaseShift = 8;
    if (selectedQuality == ResamplerQuality::Low) {
        filterSize = 8;
        phaseShift = 6;
    } else if (selectedQuality == ResamplerQuality::High) {
        filterSize = 32;
        phaseShift = 10;
    } else if (selectedQuality == ResamplerQuality::VeryHigh) {
        filterSize = 64;
        phaseShift = 12;
    }
    av_opt_set_int(swrContext.get(), "filter_size", filterSize, 0);
    av_opt_set_int(swrContext.get(), "phase_shift", phaseShift, 0);
    av_opt_set_int(swrContext.get(), "linear_interp", 0, 0);

    const int inputFrames = static_cast<int>(*frames);
    const int outputCapacity = static_cast<int>(
        av_rescale_rnd(inputFrames, targetRate, sourceRate, AV_ROUND_UP) + 32);
    std::vector<float> output(static_cast<size_t>(outputCapacity) * 2U, 0.0F);
    const uint8_t* inputData = reinterpret_cast<const uint8_t*>(stereoData->data());
    uint8_t* outputData = reinterpret_cast<uint8_t*>(output.data());
    const int converted = swr_convert(
        swrContext.get(),
        &outputData,
        outputCapacity,
        &inputData,
        inputFrames);
    if (converted <= 0) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("FFmpeg resampler produced no audio.");
        }
        return false;
    }

    output.resize(static_cast<size_t>(converted) * 2U);
    *frames = static_cast<size_t>(converted);
    *stereoData = std::move(output);
    return true;
}

#if SUPERLOOPER_HAS_SOXR
bool resampleWithSoxr(
    std::vector<float>* stereoData,
    size_t* frames,
    uint32_t sourceRate,
    uint32_t targetRate,
    ResamplerQuality quality,
    QString* errorMessage)
{
    const size_t outputFrames = std::max<size_t>(
        1U,
        static_cast<size_t>(std::ceil((static_cast<double>(*frames) * static_cast<double>(targetRate)) / static_cast<double>(sourceRate))) + 8U);
    std::vector<float> output(outputFrames * 2U, 0.0F);
    size_t inputDone = 0;
    size_t outputDone = 0;
    soxr_io_spec_t ioSpec = soxr_io_spec(SOXR_FLOAT32_I, SOXR_FLOAT32_I);
    unsigned long recipe = SOXR_VHQ;
    switch (effectiveQuality(ResamplerBackend::Soxr, quality)) {
    case ResamplerQuality::Low:
        recipe = SOXR_LQ;
        break;
    case ResamplerQuality::Medium:
        recipe = SOXR_MQ;
        break;
    case ResamplerQuality::High:
        recipe = SOXR_HQ;
        break;
    case ResamplerQuality::Automatic:
    case ResamplerQuality::VeryHigh:
        recipe = SOXR_VHQ;
        break;
    }
    soxr_quality_spec_t qualitySpec = soxr_quality_spec(recipe, 0);
    const soxr_error_t error = soxr_oneshot(
        static_cast<double>(sourceRate),
        static_cast<double>(targetRate),
        2U,
        stereoData->data(),
        *frames,
        &inputDone,
        output.data(),
        outputFrames,
        &outputDone,
        &ioSpec,
        &qualitySpec,
        nullptr);
    if (error != nullptr || outputDone == 0U) {
        if (errorMessage != nullptr) {
            *errorMessage = error != nullptr
                ? QString::fromLocal8Bit(soxr_strerror(error))
                : QStringLiteral("soxr produced no audio.");
        }
        return false;
    }

    output.resize(outputDone * 2U);
    *frames = outputDone;
    *stereoData = std::move(output);
    return true;
}
#endif

#if SUPERLOOPER_HAS_LIBSAMPLERATE
bool resampleWithLibsamplerate(
    std::vector<float>* stereoData,
    size_t* frames,
    uint32_t sourceRate,
    uint32_t targetRate,
    ResamplerQuality quality,
    QString* errorMessage)
{
    const size_t outputFrames = std::max<size_t>(
        1U,
        static_cast<size_t>(std::ceil((static_cast<double>(*frames) * static_cast<double>(targetRate)) / static_cast<double>(sourceRate))) + 8U);
    std::vector<float> output(outputFrames * 2U, 0.0F);
    SRC_DATA data {};
    data.data_in = stereoData->data();
    data.input_frames = static_cast<long>(*frames);
    data.data_out = output.data();
    data.output_frames = static_cast<long>(outputFrames);
    data.src_ratio = static_cast<double>(targetRate) / static_cast<double>(sourceRate);
    int converter = SRC_SINC_BEST_QUALITY;
    switch (effectiveQuality(ResamplerBackend::Libsamplerate, quality)) {
    case ResamplerQuality::Low:
        converter = SRC_ZERO_ORDER_HOLD;
        break;
    case ResamplerQuality::Medium:
        converter = SRC_SINC_FASTEST;
        break;
    case ResamplerQuality::High:
        converter = SRC_SINC_MEDIUM_QUALITY;
        break;
    case ResamplerQuality::Automatic:
    case ResamplerQuality::VeryHigh:
        converter = SRC_SINC_BEST_QUALITY;
        break;
    }
    const int error = src_simple(&data, converter, 2);
    if (error != 0 || data.output_frames_gen <= 0) {
        if (errorMessage != nullptr) {
            *errorMessage = QString::fromLocal8Bit(src_strerror(error));
        }
        return false;
    }

    output.resize(static_cast<size_t>(data.output_frames_gen) * 2U);
    *frames = static_cast<size_t>(data.output_frames_gen);
    *stereoData = std::move(output);
    return true;
}
#endif

bool resampleWithRubberBand(
    std::vector<float>* stereoData,
    size_t* frames,
    uint32_t sourceRate,
    uint32_t targetRate,
    ResamplerQuality quality,
    QString* errorMessage)
{
    try {
        const size_t inputFrames = *frames;
        std::vector<float> left(*frames, 0.0F);
        std::vector<float> right(*frames, 0.0F);
        for (size_t frame = 0; frame < *frames; ++frame) {
            left[frame] = (*stereoData)[frame * 2U];
            right[frame] = (*stereoData)[(frame * 2U) + 1U];
        }

        const size_t targetFrames = std::max<size_t>(
            1U,
            static_cast<size_t>(std::llround((static_cast<double>(*frames) * static_cast<double>(targetRate)) / static_cast<double>(sourceRate))));
        const double ratio = static_cast<double>(targetFrames) / static_cast<double>(*frames);
        const double pitchScale = static_cast<double>(sourceRate) / static_cast<double>(targetRate);
        auto options = RubberBand::RubberBandStretcher::OptionProcessOffline
            | RubberBand::RubberBandStretcher::OptionStretchPrecise
            | RubberBand::RubberBandStretcher::OptionChannelsTogether
            | RubberBand::RubberBandStretcher::OptionThreadingNever;
        switch (effectiveQuality(ResamplerBackend::RubberBand, quality)) {
        case ResamplerQuality::Low:
            options |= RubberBand::RubberBandStretcher::OptionWindowShort;
            break;
        case ResamplerQuality::Medium:
            options |= RubberBand::RubberBandStretcher::OptionTransientsMixed;
            break;
        case ResamplerQuality::High:
            options |= RubberBand::RubberBandStretcher::OptionEngineFiner
                | RubberBand::RubberBandStretcher::OptionTransientsCrisp;
            break;
        case ResamplerQuality::Automatic:
        case ResamplerQuality::VeryHigh:
            options |= RubberBand::RubberBandStretcher::OptionEngineFiner
                | RubberBand::RubberBandStretcher::OptionPitchHighQuality
                | RubberBand::RubberBandStretcher::OptionTransientsCrisp;
            break;
        }
        RubberBand::RubberBandStretcher stretcher(
            sourceRate,
            2,
            options,
            ratio,
            pitchScale);
        stretcher.setExpectedInputDuration(inputFrames);
        const size_t maxChunkFrames = std::min<size_t>(65536U, stretcher.getProcessSizeLimit());

        for (size_t offset = 0; offset < inputFrames; offset += maxChunkFrames) {
            const size_t chunkFrames = std::min(maxChunkFrames, inputFrames - offset);
            std::array<const float*, 2> studyInput {
                left.data() + offset,
                right.data() + offset,
            };
            const bool finalChunk = (offset + chunkFrames) >= inputFrames;
            stretcher.study(studyInput.data(), chunkFrames, finalChunk);
        }

        std::vector<float> outLeft;
        std::vector<float> outRight;
        outLeft.reserve(targetFrames + 4096U);
        outRight.reserve(targetFrames + 4096U);

        auto retrieveAvailable = [&stretcher, &outLeft, &outRight]() {
            while (true) {
                const int available = stretcher.available();
                if (available <= 0) {
                    break;
                }

                std::vector<float> chunkLeft(static_cast<size_t>(available), 0.0F);
                std::vector<float> chunkRight(static_cast<size_t>(available), 0.0F);
                std::array<float*, 2> output {
                    chunkLeft.data(),
                    chunkRight.data(),
                };
                const size_t retrieved = stretcher.retrieve(output.data(), static_cast<size_t>(available));
                if (retrieved == 0U) {
                    break;
                }

                outLeft.insert(outLeft.end(), chunkLeft.cbegin(), chunkLeft.cbegin() + static_cast<std::ptrdiff_t>(retrieved));
                outRight.insert(outRight.end(), chunkRight.cbegin(), chunkRight.cbegin() + static_cast<std::ptrdiff_t>(retrieved));
            }
        };

        for (size_t offset = 0; offset < inputFrames; offset += maxChunkFrames) {
            const size_t chunkFrames = std::min(maxChunkFrames, inputFrames - offset);
            std::array<const float*, 2> processInput {
                left.data() + offset,
                right.data() + offset,
            };
            const bool finalChunk = (offset + chunkFrames) >= inputFrames;
            stretcher.process(processInput.data(), chunkFrames, finalChunk);
            retrieveAvailable();
        }

        retrieveAvailable();
        if (outLeft.empty() || outRight.empty()) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("RubberBand produced no audio.");
            }
            return false;
        }

        const size_t outputFrames = std::min(outLeft.size(), outRight.size());
        std::vector<float> output(outputFrames * 2U, 0.0F);
        for (size_t frame = 0; frame < outputFrames; ++frame) {
            output[frame * 2U] = outLeft[frame];
            output[(frame * 2U) + 1U] = outRight[frame];
        }
        *stereoData = std::move(output);
        *frames = outputFrames;
        return true;
    } catch (const std::exception& error) {
        if (errorMessage != nullptr) {
            *errorMessage = QString::fromLocal8Bit(error.what());
        }
        return false;
    }
}

LoadedSample makeError(const QString& filePath, const QString& message)
{
    LoadedSample result;
    result.ok = false;
    result.name = QFileInfo(filePath).fileName();
    result.sourceFilePath = filePath;
    result.errorMessage = message;
    return result;
}

LoadedSample loadWithSndfile(const QString& filePath)
{
    SF_INFO info {};
    SNDFILE* file = sf_open(filePath.toLocal8Bit().constData(), SFM_READ, &info);
    if (file == nullptr) {
        return makeError(filePath, QString::fromLocal8Bit(sf_strerror(nullptr)));
    }

    if (info.frames <= 0 || info.channels <= 0 || info.samplerate <= 0) {
        sf_close(file);
        return makeError(filePath, QStringLiteral("Unsupported or empty audio file."));
    }

    const auto frameCount = static_cast<size_t>(info.frames);
    const auto channelCount = static_cast<size_t>(info.channels);

    std::vector<float> source(frameCount * channelCount);
    const sf_count_t framesRead = sf_readf_float(file, source.data(), info.frames);
    sf_close(file);

    if (framesRead <= 0) {
        return makeError(filePath, QStringLiteral("No audio frames could be read."));
    }

    const auto readableFrames = static_cast<size_t>(framesRead);
    std::vector<float> stereoData(readableFrames * 2U, 0.0F);
    for (size_t frame = 0; frame < readableFrames; ++frame) {
        const size_t sourceOffset = frame * channelCount;
        const float left = source[sourceOffset];
        const float right = channelCount > 1U ? source[sourceOffset + 1U] : left;
        stereoData[(frame * 2U)] = std::clamp(left, -1.0F, 1.0F);
        stereoData[(frame * 2U) + 1U] = std::clamp(right, -1.0F, 1.0F);
    }

    LoadedSample result;
    result.ok = true;
    result.name = QFileInfo(filePath).fileName();
    result.sourceFilePath = filePath;
    result.stereoData = std::move(stereoData);
    result.sampleRate = static_cast<uint32_t>(info.samplerate);
    result.originalSampleRate = result.sampleRate;
    result.frames = readableFrames;
    result.color = colorForAudio(result.name, result.stereoData, result.frames, result.sampleRate);
    analyzeGain(&result);
    return result;
}

LoadedSample loadWithFfmpeg(const QString& filePath)
{
    AVFormatContext* rawFormatContext = nullptr;
    if (avformat_open_input(&rawFormatContext, filePath.toLocal8Bit().constData(), nullptr, nullptr) < 0) {
        return makeError(filePath, QStringLiteral("FFmpeg could not open file."));
    }
    FormatContextPtr formatContext(rawFormatContext);

    if (avformat_find_stream_info(formatContext.get(), nullptr) < 0) {
        return makeError(filePath, QStringLiteral("FFmpeg could not read stream info."));
    }

    const int streamIndex = av_find_best_stream(formatContext.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (streamIndex < 0) {
        return makeError(filePath, QStringLiteral("No audio stream found."));
    }

    AVStream* stream = formatContext->streams[streamIndex];
    const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (decoder == nullptr) {
        return makeError(filePath, QStringLiteral("No FFmpeg decoder available."));
    }

    CodecContextPtr codecContext(avcodec_alloc_context3(decoder));
    if (!codecContext || avcodec_parameters_to_context(codecContext.get(), stream->codecpar) < 0) {
        return makeError(filePath, QStringLiteral("Could not configure FFmpeg decoder."));
    }

    if (avcodec_open2(codecContext.get(), decoder, nullptr) < 0) {
        return makeError(filePath, QStringLiteral("Could not open FFmpeg decoder."));
    }

    const int64_t inputLayout = codecContext->channel_layout != 0
        ? static_cast<int64_t>(codecContext->channel_layout)
        : av_get_default_channel_layout(codecContext->channels);
    const int64_t outputLayout = AV_CH_LAYOUT_STEREO;

    SwrContextPtr swrContext(swr_alloc_set_opts(
        nullptr,
        outputLayout,
        AV_SAMPLE_FMT_FLT,
        codecContext->sample_rate,
        inputLayout,
        codecContext->sample_fmt,
        codecContext->sample_rate,
        0,
        nullptr));
    if (!swrContext || swr_init(swrContext.get()) < 0) {
        return makeError(filePath, QStringLiteral("Could not initialize FFmpeg resampler."));
    }

    PacketPtr packet(av_packet_alloc());
    FramePtr frame(av_frame_alloc());
    if (!packet || !frame) {
        return makeError(filePath, QStringLiteral("Could not allocate FFmpeg decode buffers."));
    }

    std::vector<float> stereoData;
    auto receiveFrames = [&]() -> bool {
        while (true) {
            const int receiveStatus = avcodec_receive_frame(codecContext.get(), frame.get());
            if (receiveStatus == AVERROR(EAGAIN) || receiveStatus == AVERROR_EOF) {
                return true;
            }
            if (receiveStatus < 0) {
                return false;
            }

            const int outputSamples = swr_get_out_samples(swrContext.get(), frame->nb_samples);
            if (outputSamples <= 0) {
                av_frame_unref(frame.get());
                continue;
            }

            const size_t start = stereoData.size();
            stereoData.resize(start + (static_cast<size_t>(outputSamples) * 2U));
            uint8_t* outputData = reinterpret_cast<uint8_t*>(stereoData.data() + start);
            const int converted = swr_convert(
                swrContext.get(),
                &outputData,
                outputSamples,
                const_cast<const uint8_t**>(frame->extended_data),
                frame->nb_samples);
            if (converted < 0) {
                return false;
            }
            stereoData.resize(start + (static_cast<size_t>(converted) * 2U));
            av_frame_unref(frame.get());
        }
    };

    while (av_read_frame(formatContext.get(), packet.get()) >= 0) {
        if (packet->stream_index == streamIndex) {
            if (avcodec_send_packet(codecContext.get(), packet.get()) < 0 || !receiveFrames()) {
                av_packet_unref(packet.get());
                return makeError(filePath, QStringLiteral("FFmpeg decode failed."));
            }
        }
        av_packet_unref(packet.get());
    }

    avcodec_send_packet(codecContext.get(), nullptr);
    const bool flushed = receiveFrames();
    if (!flushed || stereoData.empty()) {
        return makeError(filePath, QStringLiteral("FFmpeg produced no audio."));
    }

    const size_t frames = stereoData.size() / 2U;
    LoadedSample result;
    result.ok = true;
    result.name = QFileInfo(filePath).fileName();
    result.sourceFilePath = filePath;
    result.stereoData = std::move(stereoData);
    result.sampleRate = static_cast<uint32_t>(codecContext->sample_rate);
    result.originalSampleRate = result.sampleRate;
    result.frames = frames;
    result.color = colorForAudio(result.name, result.stereoData, result.frames, result.sampleRate);
    analyzeGain(&result);
    return result;
}
}

SampleLoader::SampleLoader(QObject* parent)
    : QObject(parent)
{
}

void SampleLoader::loadFile(const QString& filePath)
{
    emit sampleLoaded(loadAudioFile(filePath, targetSampleRate_, resamplerBackend_, resamplerQuality_));
}

void SampleLoader::setTargetSampleRate(uint32_t sampleRate) noexcept
{
    targetSampleRate_ = sampleRate;
}

void SampleLoader::setResamplerBackend(int backend) noexcept
{
    resamplerBackend_ = static_cast<ResamplerBackend>(backend);
}

void SampleLoader::setResamplerQuality(int quality) noexcept
{
    resamplerQuality_ = static_cast<ResamplerQuality>(quality);
}

LoadedSample SampleLoader::loadAudioFile(
    const QString& filePath,
    uint32_t targetSampleRate,
    ResamplerBackend backend,
    ResamplerQuality quality)
{
    LoadedSample result = loadWithSndfile(filePath);
    if (!result.ok) {
        result = loadWithFfmpeg(filePath);
    }

    if (result.ok && targetSampleRate != 0U && result.sampleRate != targetSampleRate) {
        QString errorMessage;
        QString backendName;
        QString qualityName;
        if (!resampleStereo(
                &result.stereoData,
                &result.frames,
                &result.sampleRate,
                targetSampleRate,
                backend,
                quality,
                &errorMessage,
                &backendName,
                &qualityName)) {
            return makeError(filePath, QStringLiteral("Resampling failed: %1").arg(errorMessage));
        }
        result.resamplerName = backendName;
        result.resamplerQualityName = qualityName;
        result.resampled = true;
        result.color = colorForAudio(result.name, result.stereoData, result.frames, result.sampleRate);
        analyzeGain(&result);
    }

    return result;
}

bool SampleLoader::resampleStereo(
    std::vector<float>* stereoData,
    size_t* frames,
    uint32_t* sampleRate,
    uint32_t targetSampleRate,
    ResamplerBackend backend,
    ResamplerQuality quality,
    QString* errorMessage,
    QString* backendName,
    QString* qualityName)
{
    if (stereoData == nullptr || frames == nullptr || sampleRate == nullptr || *frames == 0U) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Invalid resampling input.");
        }
        return false;
    }

    if (*sampleRate == targetSampleRate || targetSampleRate == 0U) {
        if (backendName != nullptr) {
            *backendName = QStringLiteral("None");
        }
        if (qualityName != nullptr) {
            *qualityName = QStringLiteral("Native");
        }
        return true;
    }

    const uint32_t sourceRate = *sampleRate;
    auto tryBackend = [&](ResamplerBackend selected) -> bool {
        QString localError;
        bool ok = false;
        switch (selected) {
        case ResamplerBackend::Soxr:
#if SUPERLOOPER_HAS_SOXR
            ok = resampleWithSoxr(stereoData, frames, sourceRate, targetSampleRate, quality, &localError);
#else
            localError = QStringLiteral("soxr was not built in.");
#endif
            break;
        case ResamplerBackend::Libsamplerate:
#if SUPERLOOPER_HAS_LIBSAMPLERATE
            ok = resampleWithLibsamplerate(stereoData, frames, sourceRate, targetSampleRate, quality, &localError);
#else
            localError = QStringLiteral("libsamplerate was not built in.");
#endif
            break;
        case ResamplerBackend::RubberBand:
            ok = resampleWithRubberBand(stereoData, frames, sourceRate, targetSampleRate, quality, &localError);
            break;
        case ResamplerBackend::Ffmpeg:
            ok = resampleWithFfmpeg(stereoData, frames, sourceRate, targetSampleRate, quality, &localError);
            break;
        case ResamplerBackend::Automatic:
            break;
        }

        if (ok) {
            *sampleRate = targetSampleRate;
            if (backendName != nullptr) {
                *backendName = resamplerBackendName(selected);
            }
            if (qualityName != nullptr) {
                *qualityName = resamplerQualityName(effectiveQuality(selected, quality));
            }
            return true;
        }

        if (errorMessage != nullptr && !localError.isEmpty()) {
            *errorMessage = localError;
        }
        return false;
    };

    if (backend != ResamplerBackend::Automatic) {
        return tryBackend(backend);
    }

#if SUPERLOOPER_HAS_SOXR
    if (tryBackend(ResamplerBackend::Soxr)) {
        return true;
    }
#endif
#if SUPERLOOPER_HAS_LIBSAMPLERATE
    if (tryBackend(ResamplerBackend::Libsamplerate)) {
        return true;
    }
#endif
    if (tryBackend(ResamplerBackend::Ffmpeg)) {
        return true;
    }
    return tryBackend(ResamplerBackend::RubberBand);
}

QStringList SampleLoader::availableResamplerBackendNames()
{
    QStringList names { resamplerBackendName(ResamplerBackend::Automatic) };
#if SUPERLOOPER_HAS_SOXR
    names.push_back(resamplerBackendName(ResamplerBackend::Soxr));
#endif
    names.push_back(resamplerBackendName(ResamplerBackend::Ffmpeg));
#if SUPERLOOPER_HAS_LIBSAMPLERATE
    names.push_back(resamplerBackendName(ResamplerBackend::Libsamplerate));
#endif
    names.push_back(resamplerBackendName(ResamplerBackend::RubberBand));
    return names;
}

QStringList SampleLoader::availableResamplerQualityNames(ResamplerBackend backend)
{
    if (backend == ResamplerBackend::Automatic) {
        return QStringList { resamplerQualityName(ResamplerQuality::Automatic) };
    }

    return QStringList {
        resamplerQualityName(ResamplerQuality::Low),
        resamplerQualityName(ResamplerQuality::Medium),
        resamplerQualityName(ResamplerQuality::High),
        resamplerQualityName(ResamplerQuality::VeryHigh),
    };
}

QString SampleLoader::resamplerBackendName(ResamplerBackend backend)
{
    switch (backend) {
    case ResamplerBackend::Soxr:
        return QStringLiteral("soxr");
    case ResamplerBackend::Ffmpeg:
        return QStringLiteral("FFmpeg swresample");
    case ResamplerBackend::Libsamplerate:
        return QStringLiteral("libsamplerate");
    case ResamplerBackend::RubberBand:
        return QStringLiteral("RubberBand");
    case ResamplerBackend::Automatic:
        return QStringLiteral("Automatic");
    }
    return QStringLiteral("Automatic");
}

ResamplerBackend SampleLoader::resamplerBackendFromName(const QString& name)
{
    const QString lowerName = name.toLower();
    if (lowerName == QStringLiteral("soxr")) {
        return ResamplerBackend::Soxr;
    }
    if (lowerName.contains(QStringLiteral("ffmpeg"))) {
        return ResamplerBackend::Ffmpeg;
    }
    if (lowerName.contains(QStringLiteral("samplerate"))) {
        return ResamplerBackend::Libsamplerate;
    }
    if (lowerName.contains(QStringLiteral("rubberband"))) {
        return ResamplerBackend::RubberBand;
    }
    return ResamplerBackend::Automatic;
}

QString SampleLoader::resamplerQualityName(ResamplerQuality quality)
{
    switch (quality) {
    case ResamplerQuality::Low:
        return QStringLiteral("Low");
    case ResamplerQuality::Medium:
        return QStringLiteral("Medium");
    case ResamplerQuality::High:
        return QStringLiteral("High");
    case ResamplerQuality::VeryHigh:
        return QStringLiteral("Very High");
    case ResamplerQuality::Automatic:
        return QStringLiteral("Highest Available");
    }
    return QStringLiteral("Highest Available");
}

ResamplerQuality SampleLoader::resamplerQualityFromName(const QString& name)
{
    const QString lowerName = name.trimmed().toLower();
    if (lowerName == QStringLiteral("low")) {
        return ResamplerQuality::Low;
    }
    if (lowerName == QStringLiteral("medium")) {
        return ResamplerQuality::Medium;
    }
    if (lowerName == QStringLiteral("high")) {
        return ResamplerQuality::High;
    }
    if (lowerName.contains(QStringLiteral("very")) || lowerName.contains(QStringLiteral("highest"))) {
        return ResamplerQuality::VeryHigh;
    }
    return ResamplerQuality::Automatic;
}

ResamplerQuality SampleLoader::bestQualityForBackend(ResamplerBackend backend)
{
    switch (backend) {
    case ResamplerBackend::Automatic:
    case ResamplerBackend::Soxr:
    case ResamplerBackend::Ffmpeg:
    case ResamplerBackend::Libsamplerate:
    case ResamplerBackend::RubberBand:
        return ResamplerQuality::VeryHigh;
    }
    return ResamplerQuality::VeryHigh;
}
