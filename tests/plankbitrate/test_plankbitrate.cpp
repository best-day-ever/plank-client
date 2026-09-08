#include <QtTest>

#include "streamingpreferences.h"

class TestPlankBitrate : public QObject
{
    Q_OBJECT

private slots:
    void selectsCodecFamilyDefaults();
    void validatesCaptureProfileTuples();
    void validatesNvencH264VirtualModes();
    void retainsIndependentProfileValues();
    void clampsProtocolRange();
    void isolatesAppleProfile();
    void retainsValuesWhenAProfileIsAdded();
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

void TestPlankBitrate::validatesNvencH264VirtualModes()
{
    QVERIFY(!StreamingPreferences::isPlankVirtualModeValidForProfile(
                QStringLiteral("5120x2160"),
                StreamingPreferences::PLANK_PROFILE_NVENC_H264_8BIT_444));
    QVERIFY(StreamingPreferences::isPlankVirtualModeValidForProfile(
                QStringLiteral("4096x2160"),
                StreamingPreferences::PLANK_PROFILE_NVENC_H264_8BIT_444));
    QVERIFY(!StreamingPreferences::isPlankVirtualModeValidForProfile(
                QStringLiteral("4096x2161"),
                StreamingPreferences::PLANK_PROFILE_NVENC_H264_8BIT_444));
    QVERIFY(StreamingPreferences::isPlankVirtualModeValidForProfile(
                QStringLiteral("5120x2160"),
                StreamingPreferences::PLANK_PROFILE_H264_10BIT_444));
    QVERIFY(StreamingPreferences::isPlankVirtualModeValidForProfile(
                QStringLiteral("5120x2160"),
                StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_8BIT_444));
    QVERIFY(StreamingPreferences::isPlankVirtualModeValidForProfile(
                QStringLiteral("5120x2160"),
                StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_10BIT_444));
    QVERIFY(!StreamingPreferences::isPlankVirtualModeValidForProfile(
                QStringLiteral("not-a-mode"),
                StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_10BIT_444));
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

void TestPlankBitrate::isolatesAppleProfile()
{
    using P = StreamingPreferences;
    QCOMPARE(int(P::PLANK_PROFILE_NVENC_HEVC_10BIT_444), 6);
    QCOMPARE(int(P::PLANK_PROFILE_APPLE_HEVC_10BIT_420), 7);
    QCOMPARE(int(P::PLANK_PROFILE_APPLE_HEVC_10BIT_444), 8);
    QCOMPARE(int(P::PLANK_CAPTURE_SCREENCAPTUREKIT), 2);
    for (int profile = 0; profile < P::PLANK_PROFILE_COUNT; ++profile) {
        const bool apple = P::isPlankAppleProfile(profile);
        QCOMPARE(P::isPlankProfileValidForCaptureSource(profile, P::PLANK_CAPTURE_SCREENCAPTUREKIT), apple);
        QCOMPARE(P::isPlankProfileValidForCaptureSource(profile, P::PLANK_CAPTURE_NVFBC_8BIT), !apple);
    }
    QVERIFY(!P::isPlankNvencProfile(P::PLANK_PROFILE_APPLE_HEVC_10BIT_420));
    QVERIFY(!P::isPlankProfileValidForCaptureSource(P::PLANK_PROFILE_APPLE_HEVC_10BIT_420, P::PLANK_CAPTURE_X11_NATIVE10));
    QVERIFY(!P::isPlankProfileValidForCaptureSource(P::PLANK_PROFILE_COUNT, P::PLANK_CAPTURE_SCREENCAPTUREKIT));
    QVERIFY(!P::isPlankProfileValidForCaptureSource(-1, P::PLANK_CAPTURE_SCREENCAPTUREKIT));
    for (const auto& mode : {"1920x1080", "2560x1600", "3840x2160", "4096x2160", "5120x2160"}) {
        QVERIFY(P::isPlankVirtualModeValidForProfile(QString::fromLatin1(mode), P::PLANK_PROFILE_APPLE_HEVC_10BIT_420));
        QVERIFY(P::isPlankVirtualModeValidForProfile(QString::fromLatin1(mode), P::PLANK_PROFILE_APPLE_HEVC_10BIT_444));
    }
    for (const auto& mode : {"5121x2160", "5120x2161", "7680x4320", "invalid"}) {
        QVERIFY(!P::isPlankVirtualModeValidForProfile(QString::fromLatin1(mode), P::PLANK_PROFILE_APPLE_HEVC_10BIT_420));
        QVERIFY(!P::isPlankVirtualModeValidForProfile(QString::fromLatin1(mode), P::PLANK_PROFILE_APPLE_HEVC_10BIT_444));
    }
    QCOMPARE(P::plankDefaultBitrateForProfile(P::PLANK_PROFILE_APPLE_HEVC_10BIT_420), P::PlankHevcDefaultBitrateKbps);
    QCOMPARE(P::plankDefaultBitrateForProfile(P::PLANK_PROFILE_APPLE_HEVC_10BIT_444), P::PlankHevcDefaultBitrateKbps);
    QVERIFY(!P::isPlankNvencProfile(P::PLANK_PROFILE_APPLE_HEVC_10BIT_444));
    QVERIFY(P::plankAppleEncodingMode(P::PLANK_PROFILE_H264_10BIT_444).isEmpty());
}

void TestPlankBitrate::retainsValuesWhenAProfileIsAdded()
{
    using P = StreamingPreferences;
    const QVariantList saved{76500, 68500, 99000, 10000, 150000, 42500, 51000};
    QVector<int> parsed;
    QVERIFY(P::plankProfileBitratesFromVariantList(saved, parsed));
    QCOMPARE(parsed.size(), int(P::PLANK_PROFILE_COUNT));
    for (int i = 0; i < saved.size(); ++i) QCOMPARE(parsed[i], saved[i].toInt());
    QCOMPARE(parsed.last(), P::PlankHevcDefaultBitrateKbps);
    const auto unchanged = parsed;
    auto invalid = saved;
    invalid[2] = 999999;
    QVERIFY(!P::plankProfileBitratesFromVariantList(invalid, parsed));
    QCOMPARE(parsed, unchanged);
    QVERIFY(!P::plankProfileBitratesFromVariantList({}, parsed));
    invalid = P::plankProfileBitratesToVariantList(parsed);
    invalid.append(50000);
    QVERIFY(!P::plankProfileBitratesFromVariantList(invalid, parsed));
    QCOMPARE(parsed, unchanged);
}

QTEST_APPLESS_MAIN(TestPlankBitrate)

#include "test_plankbitrate.moc"
