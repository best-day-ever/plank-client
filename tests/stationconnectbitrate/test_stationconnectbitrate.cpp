#include <QtTest>

#include "streamingpreferences.h"

class TestStationConnectBitrate : public QObject
{
    Q_OBJECT

private slots:
    void selectsCodecFamilyDefaults();
    void validatesCaptureProfileTuples();
    void retainsIndependentProfileValues();
    void clampsProtocolRange();
};

void TestStationConnectBitrate::selectsCodecFamilyDefaults()
{
    const int h264Profiles[] = {
        StreamingPreferences::SCVP_H264_10BIT_444,
        StreamingPreferences::SCVP_H264_8BIT_422,
        StreamingPreferences::SCVP_H264_8BIT_444,
        StreamingPreferences::SCVP_H264_10BIT_422,
        StreamingPreferences::SCVP_NVENC_H264_8BIT_444,
    };
    for (const int profile : h264Profiles) {
        QCOMPARE(
            StreamingPreferences::stationConnectDefaultBitrateForProfile(profile),
            StreamingPreferences::StationConnectH264DefaultBitrateKbps);
    }

    QCOMPARE(
        StreamingPreferences::stationConnectDefaultBitrateForProfile(
            StreamingPreferences::SCVP_NVENC_HEVC_8BIT_444),
        StreamingPreferences::StationConnectHevcDefaultBitrateKbps);
    QCOMPARE(
        StreamingPreferences::stationConnectDefaultBitrateForProfile(
            StreamingPreferences::SCVP_NVENC_HEVC_10BIT_444),
        StreamingPreferences::StationConnectHevcDefaultBitrateKbps);
}

void TestStationConnectBitrate::validatesCaptureProfileTuples()
{
    QVERIFY(StreamingPreferences::isStationConnectProfileValidForCaptureSource(
                StreamingPreferences::SCVP_NVENC_HEVC_10BIT_444,
                StreamingPreferences::SCCS_NVFBC_8BIT));
    QVERIFY(StreamingPreferences::isStationConnectProfileValidForCaptureSource(
                StreamingPreferences::SCVP_NVENC_HEVC_10BIT_444,
                StreamingPreferences::SCCS_X11_NATIVE10));
    QVERIFY(!StreamingPreferences::isStationConnectProfileValidForCaptureSource(
                StreamingPreferences::SCVP_NVENC_HEVC_8BIT_444,
                StreamingPreferences::SCCS_X11_NATIVE10));
}

void TestStationConnectBitrate::retainsIndependentProfileValues()
{
    QVector<int> bitrates =
            StreamingPreferences::stationConnectDefaultProfileBitrates();
    bitrates[StreamingPreferences::SCVP_H264_10BIT_444] = 76500;
    bitrates[StreamingPreferences::SCVP_NVENC_H264_8BIT_444] = 68000;
    bitrates[StreamingPreferences::SCVP_NVENC_HEVC_8BIT_444] = 42500;
    bitrates[StreamingPreferences::SCVP_NVENC_HEVC_10BIT_444] = 51000;

    QCOMPARE(StreamingPreferences::stationConnectBitrateForProfile(
                 bitrates, StreamingPreferences::SCVP_H264_10BIT_444),
             76500);
    QCOMPARE(StreamingPreferences::stationConnectBitrateForProfile(
                 bitrates, StreamingPreferences::SCVP_NVENC_H264_8BIT_444),
             68000);
    QCOMPARE(StreamingPreferences::stationConnectBitrateForProfile(
                 bitrates, StreamingPreferences::SCVP_NVENC_HEVC_8BIT_444),
             42500);
    QCOMPARE(StreamingPreferences::stationConnectBitrateForProfile(
                 bitrates, StreamingPreferences::SCVP_NVENC_HEVC_10BIT_444),
             51000);

    QVector<int> roundTripped;
    QVERIFY(StreamingPreferences::stationConnectProfileBitratesFromVariantList(
                StreamingPreferences::stationConnectProfileBitratesToVariantList(
                    bitrates),
                roundTripped));
    QCOMPARE(roundTripped, bitrates);
}

void TestStationConnectBitrate::clampsProtocolRange()
{
    QCOMPARE(StreamingPreferences::clampStationConnectBitrate(1),
             StreamingPreferences::StationConnectBitrateMinimumKbps);
    QCOMPARE(StreamingPreferences::clampStationConnectBitrate(76500), 76500);
    QCOMPARE(StreamingPreferences::clampStationConnectBitrate(999999),
             StreamingPreferences::StationConnectBitrateMaximumKbps);
}

QTEST_APPLESS_MAIN(TestStationConnectBitrate)

#include "test_stationconnectbitrate.moc"
