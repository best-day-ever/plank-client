#include <QtTest>

#include "streamingpreferences.h"

class TestPlankBitrate : public QObject
{
    Q_OBJECT

private slots:
    void selectsCodecFamilyDefaults();
    void validatesCaptureProfileTuples();
    void retainsIndependentProfileValues();
    void clampsProtocolRange();
};

void TestPlankBitrate::selectsCodecFamilyDefaults()
{
    const int h264Profiles[] = {
        StreamingPreferences::PLANK_PROFILE_H264_10BIT_444,
        StreamingPreferences::PLANK_PROFILE_H264_8BIT_422,
        StreamingPreferences::PLANK_PROFILE_H264_8BIT_444,
        StreamingPreferences::PLANK_PROFILE_H264_10BIT_422,
        StreamingPreferences::PLANK_PROFILE_NVENC_H264_8BIT_444,
    };
    for (const int profile : h264Profiles) {
        QCOMPARE(
            StreamingPreferences::plankDefaultBitrateForProfile(profile),
            StreamingPreferences::PlankH264DefaultBitrateKbps);
    }

    QCOMPARE(
        StreamingPreferences::plankDefaultBitrateForProfile(
            StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_8BIT_444),
        StreamingPreferences::PlankHevcDefaultBitrateKbps);
    QCOMPARE(
        StreamingPreferences::plankDefaultBitrateForProfile(
            StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_10BIT_444),
        StreamingPreferences::PlankHevcDefaultBitrateKbps);
}

void TestPlankBitrate::validatesCaptureProfileTuples()
{
    QVERIFY(StreamingPreferences::isPlankProfileValidForCaptureSource(
                StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_10BIT_444,
                StreamingPreferences::PLANK_CAPTURE_NVFBC_8BIT));
    QVERIFY(StreamingPreferences::isPlankProfileValidForCaptureSource(
                StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_10BIT_444,
                StreamingPreferences::PLANK_CAPTURE_X11_NATIVE10));
    QVERIFY(!StreamingPreferences::isPlankProfileValidForCaptureSource(
                StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_8BIT_444,
                StreamingPreferences::PLANK_CAPTURE_X11_NATIVE10));
}

void TestPlankBitrate::retainsIndependentProfileValues()
{
    QVector<int> bitrates =
            StreamingPreferences::plankDefaultProfileBitrates();
    bitrates[StreamingPreferences::PLANK_PROFILE_H264_10BIT_444] = 76500;
    bitrates[StreamingPreferences::PLANK_PROFILE_NVENC_H264_8BIT_444] = 68000;
    bitrates[StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_8BIT_444] = 42500;
    bitrates[StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_10BIT_444] = 51000;

    QCOMPARE(StreamingPreferences::plankBitrateForProfile(
                 bitrates, StreamingPreferences::PLANK_PROFILE_H264_10BIT_444),
             76500);
    QCOMPARE(StreamingPreferences::plankBitrateForProfile(
                 bitrates, StreamingPreferences::PLANK_PROFILE_NVENC_H264_8BIT_444),
             68000);
    QCOMPARE(StreamingPreferences::plankBitrateForProfile(
                 bitrates, StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_8BIT_444),
             42500);
    QCOMPARE(StreamingPreferences::plankBitrateForProfile(
                 bitrates, StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_10BIT_444),
             51000);

    QVector<int> roundTripped;
    QVERIFY(StreamingPreferences::plankProfileBitratesFromVariantList(
                StreamingPreferences::plankProfileBitratesToVariantList(
                    bitrates),
                roundTripped));
    QCOMPARE(roundTripped, bitrates);
}

void TestPlankBitrate::clampsProtocolRange()
{
    QCOMPARE(StreamingPreferences::clampPlankBitrate(1),
             StreamingPreferences::PlankBitrateMinimumKbps);
    QCOMPARE(StreamingPreferences::clampPlankBitrate(76500), 76500);
    QCOMPARE(StreamingPreferences::clampPlankBitrate(999999),
             StreamingPreferences::PlankBitrateMaximumKbps);
}

QTEST_APPLESS_MAIN(TestPlankBitrate)

#include "test_plankbitrate.moc"
