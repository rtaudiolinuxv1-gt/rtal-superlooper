#include <array>
#include <vector>

#include <QtTest/QtTest>

#include "../src/LoopManager.h"

namespace {
Sample makeSample(std::initializer_list<float> interleavedStereo)
{
    Sample sample;
    sample.data = interleavedStereo;
    sample.sampleRate = 48000;
    sample.frames = sample.data.size() / 2U;
    sample.name = QStringLiteral("test");
    return sample;
}
}

class LoopManagerTest final : public QObject
{
    Q_OBJECT

private slots:
    void playbackLoopsAssignedSample();
    void secondToggleStopsAtLoopBoundary();
    void fixedLengthRecordingCopiesInput();
    void holdRecordingStopsOnRelease();
    void autoRecordingStartsOnSignalAndTrimsSilence();
    void layeredPlaybackMixesBothSamples();
    void playbackUsesSampleTrimRange();
    void layeredPlaybackUsesSampleTrimRanges();
    void stopAllPlaybackSilencesActiveSamples();
    void keyVolumeScalesPlayback();
    void pauseToggleStopsImmediatelyAndResumesFromCurrentFrame();
    void masterGainAndLimiterProtectOutput();
    void panMuteSoloGroupAndNormalizationAffectMix();
    void fadeInRampsPlaybackStart();
    void noLoopSelfMixAllowsOverlap();
    void heldNoLoopNotePlaysUntilReleased();
    void releaseTriggeredVirtualStaccatoAddsTailThenFades();
};

void LoopManagerTest::playbackLoopsAssignedSample()
{
    LoopManager manager;
    Sample sample = makeSample({
        0.10F, 0.20F,
        0.30F, 0.40F,
    });

    std::array<float, 5> left {};
    std::array<float, 5> right {};

    manager.assignSample(0, &sample);
    manager.togglePlayback(0);
    manager.process(5, nullptr, nullptr, left.data(), right.data());

    QCOMPARE(left[0], 0.10F);
    QCOMPARE(right[0], 0.20F);
    QCOMPARE(left[1], 0.30F);
    QCOMPARE(right[1], 0.40F);
    QCOMPARE(left[2], 0.10F);
    QCOMPARE(right[2], 0.20F);
}

void LoopManagerTest::secondToggleStopsAtLoopBoundary()
{
    LoopManager manager;
    Sample sample = makeSample({
        1.00F, 1.00F,
        0.50F, 0.50F,
    });

    std::array<float, 4> left {};
    std::array<float, 4> right {};

    manager.assignSample(5, &sample);
    manager.togglePlayback(5);
    manager.process(1, nullptr, nullptr, left.data(), right.data());
    manager.togglePlayback(5);
    manager.process(4, nullptr, nullptr, left.data(), right.data());

    QCOMPARE(left[0], 0.50F);
    QCOMPARE(left[1], 0.00F);
    QCOMPARE(left[2], 0.00F);
    QCOMPARE(left[3], 0.00F);
}

void LoopManagerTest::fixedLengthRecordingCopiesInput()
{
    LoopManager manager;
    manager.setSampleRate(10);

    std::array<float, 5> inputLeft { 0.10F, 0.20F, 0.30F, 0.40F, 0.50F };
    std::array<float, 5> inputRight { -0.10F, -0.20F, -0.30F, -0.40F, -0.50F };
    std::array<float, 5> outputLeft {};
    std::array<float, 5> outputRight {};

    manager.startRecording(7, 1.0, false);
    manager.setRecordingTarget(7, 0.4);
    manager.process(5, inputLeft.data(), inputRight.data(), outputLeft.data(), outputRight.data());

    FinishedRecording recording;
    QVERIFY(manager.takeFinishedRecording(&recording));
    QCOMPARE(recording.noteIndex, 7);
    QCOMPARE(recording.sampleRate, 10U);
    QCOMPARE(recording.frames, 4U);
    QCOMPARE(recording.data.size(), 8U);
    QCOMPARE(recording.data[0], 0.10F);
    QCOMPARE(recording.data[1], -0.10F);
    QCOMPARE(recording.data[6], 0.40F);
    QCOMPARE(recording.data[7], -0.40F);
}

void LoopManagerTest::holdRecordingStopsOnRelease()
{
    LoopManager manager;
    manager.setSampleRate(10);

    std::array<float, 3> inputLeft { 0.25F, 0.50F, 0.75F };
    std::array<float, 3> inputRight { 0.10F, 0.20F, 0.30F };
    std::array<float, 3> outputLeft {};
    std::array<float, 3> outputRight {};

    manager.startRecording(9, 1.0, false);
    manager.process(3, inputLeft.data(), inputRight.data(), outputLeft.data(), outputRight.data());
    manager.stopRecording(9);
    manager.process(3, inputLeft.data(), inputRight.data(), outputLeft.data(), outputRight.data());

    FinishedRecording recording;
    QVERIFY(manager.takeFinishedRecording(&recording));
    QCOMPARE(recording.noteIndex, 9);
    QCOMPARE(recording.frames, 3U);
    QCOMPARE(recording.data[4], 0.75F);
    QCOMPARE(recording.data[5], 0.30F);
}

void LoopManagerTest::autoRecordingStartsOnSignalAndTrimsSilence()
{
    LoopManager manager;
    manager.setSampleRate(100);

    std::array<float, 50> inputLeft {};
    std::array<float, 50> inputRight {};
    for (size_t frame = 5; frame < 15; ++frame) {
        inputLeft[frame] = 0.10F;
        inputRight[frame] = 0.10F;
    }

    std::array<float, 50> outputLeft {};
    std::array<float, 50> outputRight {};

    manager.startRecording(11, 1.0, true);
    manager.process(50, inputLeft.data(), inputRight.data(), outputLeft.data(), outputRight.data());

    FinishedRecording recording;
    QVERIFY(manager.takeFinishedRecording(&recording));
    QCOMPARE(recording.noteIndex, 11);
    QCOMPARE(recording.frames, 10U);
    QCOMPARE(recording.data.front(), 0.10F);
    QCOMPARE(recording.data.back(), 0.10F);
}

void LoopManagerTest::layeredPlaybackMixesBothSamples()
{
    LoopManager manager;
    manager.setLimiterEnabled(false);
    Sample first = makeSample({
        0.10F, 0.20F,
        0.30F, 0.40F,
    });
    Sample second = makeSample({
        1.00F, 2.00F,
        3.00F, 4.00F,
    });

    std::array<float, 2> left {};
    std::array<float, 2> right {};

    manager.playLayered(&first, &second);
    manager.process(2, nullptr, nullptr, left.data(), right.data());

    QCOMPARE(left[0], 1.10F);
    QCOMPARE(right[0], 2.20F);
    QCOMPARE(left[1], 3.30F);
    QCOMPARE(right[1], 4.40F);
}

void LoopManagerTest::playbackUsesSampleTrimRange()
{
    LoopManager manager;
    Sample sample = makeSample({
        0.10F, 0.10F,
        0.20F, 0.20F,
        0.30F, 0.30F,
        0.40F, 0.40F,
    });
    sample.startFrame = 0.25;
    sample.endFrame = 0.75;

    std::array<float, 4> left {};
    std::array<float, 4> right {};

    manager.assignSample(0, &sample);
    manager.togglePlayback(0);
    manager.process(4, nullptr, nullptr, left.data(), right.data());

    QCOMPARE(left[0], 0.20F);
    QCOMPARE(right[0], 0.20F);
    QCOMPARE(left[1], 0.30F);
    QCOMPARE(right[1], 0.30F);
    QCOMPARE(left[2], 0.20F);
    QCOMPARE(right[2], 0.20F);
}

void LoopManagerTest::layeredPlaybackUsesSampleTrimRanges()
{
    LoopManager manager;
    manager.setLimiterEnabled(false);
    Sample first = makeSample({
        0.10F, 0.10F,
        0.20F, 0.20F,
        0.30F, 0.30F,
    });
    Sample second = makeSample({
        1.00F, 1.00F,
        2.00F, 2.00F,
        3.00F, 3.00F,
    });
    first.startFrame = 1.0 / 3.0;
    first.endFrame = 2.0 / 3.0;
    second.startFrame = 2.0 / 3.0;
    second.endFrame = 1.0;

    std::array<float, 1> left {};
    std::array<float, 1> right {};

    manager.playLayered(&first, &second);
    manager.process(1, nullptr, nullptr, left.data(), right.data());

    QCOMPARE(left[0], 3.20F);
    QCOMPARE(right[0], 3.20F);
}

void LoopManagerTest::stopAllPlaybackSilencesActiveSamples()
{
    LoopManager manager;
    Sample first = makeSample({
        0.50F, 0.50F,
        0.50F, 0.50F,
    });
    Sample second = makeSample({
        1.00F, 1.00F,
        1.00F, 1.00F,
    });

    std::array<float, 2> left {};
    std::array<float, 2> right {};

    manager.assignSample(0, &first);
    manager.togglePlayback(0);
    manager.playLayered(&first, &second);
    manager.startPreview(&second, 0, 1, 1.0);
    manager.process(1, nullptr, nullptr, left.data(), right.data());
    QVERIFY(left[0] > 0.0F);

    left.fill(0.0F);
    right.fill(0.0F);
    manager.stopAllPlayback();
    manager.process(2, nullptr, nullptr, left.data(), right.data());

    QCOMPARE(left[0], 0.0F);
    QCOMPARE(right[0], 0.0F);
    QCOMPARE(left[1], 0.0F);
    QCOMPARE(right[1], 0.0F);
}

void LoopManagerTest::keyVolumeScalesPlayback()
{
    LoopManager manager;
    Sample sample = makeSample({
        0.50F, 0.25F,
    });

    std::array<float, 1> left {};
    std::array<float, 1> right {};

    manager.assignSample(0, &sample);
    manager.setKeyVolume(0, 0.5F);
    manager.togglePlayback(0);
    manager.process(1, nullptr, nullptr, left.data(), right.data());

    QCOMPARE(left[0], 0.25F);
    QCOMPARE(right[0], 0.125F);
}

void LoopManagerTest::pauseToggleStopsImmediatelyAndResumesFromCurrentFrame()
{
    LoopManager manager;
    Sample sample = makeSample({
        0.10F, 0.10F,
        0.20F, 0.20F,
        0.30F, 0.30F,
    });

    std::array<float, 2> left {};
    std::array<float, 2> right {};

    manager.assignSample(0, &sample);
    manager.togglePausePlayback(0);
    manager.process(2, nullptr, nullptr, left.data(), right.data());
    QCOMPARE(left[0], 0.10F);
    QCOMPARE(left[1], 0.20F);

    left.fill(0.0F);
    right.fill(0.0F);
    manager.togglePausePlayback(0);
    manager.process(2, nullptr, nullptr, left.data(), right.data());
    QCOMPARE(left[0], 0.0F);
    QCOMPARE(left[1], 0.0F);

    manager.togglePausePlayback(0);
    manager.process(1, nullptr, nullptr, left.data(), right.data());
    QCOMPARE(left[0], 0.30F);
}

void LoopManagerTest::masterGainAndLimiterProtectOutput()
{
    LoopManager manager;
    Sample sample = makeSample({
        2.00F, -2.00F,
    });

    std::array<float, 1> left {};
    std::array<float, 1> right {};

    manager.assignSample(0, &sample);
    manager.setMasterGain(0.5F);
    manager.togglePlayback(0);
    manager.process(1, nullptr, nullptr, left.data(), right.data());
    QCOMPARE(left[0], 1.0F);
    QCOMPARE(right[0], -1.0F);
    QCOMPARE(manager.outputPeakLeft(), 1.0F);

    left.fill(0.0F);
    right.fill(0.0F);
    manager.stopAllPlayback();
    manager.process(1, nullptr, nullptr, left.data(), right.data());
    manager.setMasterGain(1.0F);
    manager.assignSample(0, &sample);
    manager.togglePlayback(0);
    manager.process(1, nullptr, nullptr, left.data(), right.data());
    QVERIFY(std::abs(left[0]) < 1.0F);
    QVERIFY(manager.limiterWasActive());
}

void LoopManagerTest::panMuteSoloGroupAndNormalizationAffectMix()
{
    LoopManager manager;
    manager.setLimiterEnabled(false);
    Sample first = makeSample({
        0.50F, 0.50F,
    });
    first.normalizationGain = 2.0F;
    Sample second = makeSample({
        0.25F, 0.25F,
    });

    std::array<float, 1> left {};
    std::array<float, 1> right {};

    manager.assignSample(0, &first);
    manager.assignSample(1, &second);
    manager.setSampleNormalizationEnabled(true);
    manager.setKeyPan(0, -1.0F);
    manager.setKeyGroup(0, 1);
    manager.setGroupGain(1, 0.5F);
    manager.setKeySolo(0, true);
    manager.togglePlayback(0);
    manager.togglePlayback(1);
    manager.process(1, nullptr, nullptr, left.data(), right.data());

    QCOMPARE(left[0], 0.50F);
    QCOMPARE(right[0], 0.0F);

    left.fill(0.0F);
    right.fill(0.0F);
    manager.setKeySolo(0, false);
    manager.setKeyMuted(0, true);
    manager.process(1, nullptr, nullptr, left.data(), right.data());
    QCOMPARE(left[0], 0.25F);
    QCOMPARE(right[0], 0.25F);
}

void LoopManagerTest::fadeInRampsPlaybackStart()
{
    LoopManager manager;
    manager.setLimiterEnabled(false);
    manager.setSampleRate(1000);
    manager.setFadeTimeMs(2.0);
    Sample sample = makeSample({
        1.00F, 1.00F,
        1.00F, 1.00F,
        1.00F, 1.00F,
    });

    std::array<float, 3> left {};
    std::array<float, 3> right {};

    manager.assignSample(0, &sample);
    manager.togglePlayback(0);
    manager.process(3, nullptr, nullptr, left.data(), right.data());

    QCOMPARE(left[0], 0.0F);
    QCOMPARE(left[1], 0.5F);
    QCOMPARE(left[2], 1.0F);
}

void LoopManagerTest::noLoopSelfMixAllowsOverlap()
{
    LoopManager manager;
    manager.setLimiterEnabled(false);
    Sample sample = makeSample({
        1.00F, 1.00F,
        1.00F, 1.00F,
    });

    std::array<float, 1> left {};
    std::array<float, 1> right {};

    manager.assignSample(0, &sample);
    manager.setKeyLoopMode(0, KeyLoopMode::NoLoop);
    manager.setKeySelfMixEnabled(0, true);

    manager.togglePlayback(0);
    manager.process(1, nullptr, nullptr, left.data(), right.data());
    QCOMPARE(left[0], 1.0F);

    left.fill(0.0F);
    right.fill(0.0F);
    manager.togglePlayback(0);
    manager.process(1, nullptr, nullptr, left.data(), right.data());
    QCOMPARE(left[0], 2.0F);
    QCOMPARE(right[0], 2.0F);
}

void LoopManagerTest::heldNoLoopNotePlaysUntilReleased()
{
    LoopManager manager;
    manager.setLimiterEnabled(false);
    manager.setSampleRate(100);
    Sample sample = makeSample({
        1.00F, 1.00F,
        1.00F, 1.00F,
        1.00F, 1.00F,
        1.00F, 1.00F,
    });

    std::array<float, 4> left {};
    std::array<float, 4> right {};

    manager.assignSample(0, &sample);
    manager.setKeyLoopMode(0, KeyLoopMode::NoLoop);
    manager.setKeyVirtualStaccatoEnabled(0, true);
    manager.noteOn(0);
    manager.process(4, nullptr, nullptr, left.data(), right.data());

    QCOMPARE(left[0], 1.0F);
    QCOMPARE(left[1], 1.0F);
    QCOMPARE(left[2], 1.0F);
    QCOMPARE(left[3], 1.0F);
}

void LoopManagerTest::releaseTriggeredVirtualStaccatoAddsTailThenFades()
{
    LoopManager manager;
    manager.setLimiterEnabled(false);
    manager.setSampleRate(100);
    manager.setKeyReleaseMs(0, 20.0);
    manager.setKeyStaccato(0, 1.0F);
    manager.setKeyVirtualStaccatoEnabled(0, true);

    Sample sample = makeSample({
        1.00F, 1.00F,
        1.00F, 1.00F,
        1.00F, 1.00F,
        1.00F, 1.00F,
        1.00F, 1.00F,
        1.00F, 1.00F,
        1.00F, 1.00F,
        1.00F, 1.00F,
        1.00F, 1.00F,
        1.00F, 1.00F,
    });

    std::array<float, 8> left {};
    std::array<float, 8> right {};

    manager.assignSample(0, &sample);
    manager.setKeyLoopMode(0, KeyLoopMode::NoLoop);
    manager.noteOn(0);
    manager.noteOff(0);
    manager.process(8, nullptr, nullptr, left.data(), right.data());

    QCOMPARE(left[0], 1.0F);
    QCOMPARE(left[1], 1.0F);
    QCOMPARE(left[2], 1.0F);
    QCOMPARE(left[3], 1.0F);
    QCOMPARE(left[4], 1.0F);
    QCOMPARE(left[5], 1.0F);
    QCOMPARE(left[6], 0.5F);
    QCOMPARE(left[7], 0.0F);
}

QTEST_MAIN(LoopManagerTest)

#include "test_loop_manager.moc"
