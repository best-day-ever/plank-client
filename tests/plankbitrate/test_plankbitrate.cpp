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
    void addsNvenc420ProfilesAtTheEnd();
    void padsNineProfileBitrateLists();
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
    QCOMPARE(parsed[P::PLANK_PROFILE_APPLE_HEVC_10BIT_444], P::PlankHevcDefaultBitrateKbps);
    QCOMPARE(parsed.last(), P::PlankHevcYuv420DefaultBitrateKbps);
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

void TestPlankBitrate::addsNvenc420ProfilesAtTheEnd()
{
    using P = StreamingPreferences;
    // Append-only: bookmarks store these numbers.
    QCOMPARE(int(P::PLANK_PROFILE_APPLE_HEVC_10BIT_444), 8);
    QCOMPARE(int(P::PLANK_PROFILE_NVENC_H264_8BIT_420), 9);
    QCOMPARE(int(P::PLANK_PROFILE_NVENC_HEVC_10BIT_420), 10);
    QCOMPARE(int(P::PLANK_PROFILE_COUNT), 11);

    for (int profile = 0; profile < P::PLANK_PROFILE_COUNT; ++profile) {
        const bool yuv420 = profile == P::PLANK_PROFILE_NVENC_H264_8BIT_420 ||
                            profile == P::PLANK_PROFILE_NVENC_HEVC_10BIT_420;
        QCOMPARE(P::isPlankNvenc420Profile(profile), yuv420);
        if (yuv420) {
            QVERIFY(P::isPlankNvencProfile(profile));
            QVERIFY(!P::isPlankAppleProfile(profile));
            QVERIFY(P::plankAppleEncodingMode(profile).isEmpty());
            // NvFBC 8-bit only: native 10-bit capture is identity 4:4:4.
            QVERIFY(P::isPlankProfileValidForCaptureSource(profile, P::PLANK_CAPTURE_NVFBC_8BIT));
            QVERIFY(!P::isPlankProfileValidForCaptureSource(profile, P::PLANK_CAPTURE_X11_NATIVE10));
            QVERIFY(!P::isPlankProfileValidForCaptureSource(profile, P::PLANK_CAPTURE_SCREENCAPTUREKIT));
        }
    }
    QVERIFY(!P::isPlankNvenc420Profile(P::PLANK_PROFILE_APPLE_HEVC_10BIT_420));
    QVERIFY(!P::isPlankNvenc420Profile(P::PLANK_PROFILE_COUNT));
    QVERIFY(!P::isPlankNvenc420Profile(-1));

    QVERIFY(P::isPlankH264NvencProfile(P::PLANK_PROFILE_NVENC_H264_8BIT_420));
    QVERIFY(!P::isPlankH264NvencProfile(P::PLANK_PROFILE_NVENC_HEVC_10BIT_420));
    QVERIFY(!P::isPlankVirtualModeValidForProfile(QStringLiteral("5120x2160"), P::PLANK_PROFILE_NVENC_H264_8BIT_420));
    QVERIFY(P::isPlankVirtualModeValidForProfile(QStringLiteral("4096x2160"), P::PLANK_PROFILE_NVENC_H264_8BIT_420));
    QVERIFY(P::isPlankVirtualModeValidForProfile(QStringLiteral("5120x2160"), P::PLANK_PROFILE_NVENC_HEVC_10BIT_420));

    QCOMPARE(P::plankDefaultBitrateForProfile(P::PLANK_PROFILE_NVENC_H264_8BIT_420), 50000);
    QCOMPARE(P::plankDefaultBitrateForProfile(P::PLANK_PROFILE_NVENC_HEVC_10BIT_420), 30000);
    QCOMPARE(P::plankDefaultProfileBitrates().size(), int(P::PLANK_PROFILE_COUNT));
}

void TestPlankBitrate::padsNineProfileBitrateLists()
{
    using P = StreamingPreferences;
    // A bookmark saved before the 4:2:0 profiles has nine entries. Loading it
    // keeps all nine and gives the new profiles their defaults.
    const QVariantList saved{76500, 68500, 99000, 10000, 150000, 42500, 51000, 60000, 40000};
    QVector<int> parsed;
    QVERIFY(P::plankProfileBitratesFromVariantList(saved, parsed));
    QCOMPARE(parsed.size(), int(P::PLANK_PROFILE_COUNT));
    for (int i = 0; i < saved.size(); ++i) QCOMPARE(parsed[i], saved[i].toInt());
    QCOMPARE(parsed[P::PLANK_PROFILE_NVENC_H264_8BIT_420], P::PlankH264Yuv420DefaultBitrateKbps);
    QCOMPARE(parsed[P::PLANK_PROFILE_NVENC_HEVC_10BIT_420], P::PlankHevcYuv420DefaultBitrateKbps);
    QCOMPARE(P::plankBitrateForProfile(parsed, P::PLANK_PROFILE_NVENC_HEVC_10BIT_420), 30000);
    // A short vector that was never padded still falls back to the default.
    QVector<int> nine;
    for (const QVariant& value : saved) nine.append(value.toInt());
    QCOMPARE(P::plankBitrateForProfile(nine, P::PLANK_PROFILE_NVENC_H264_8BIT_420), 50000);
    QCOMPARE(P::plankProfileBitratesToVariantList(parsed).size(), int(P::PLANK_PROFILE_COUNT));
}

QTEST_APPLESS_MAIN(TestPlankBitrate)

#include "test_plankbitrate.moc"
