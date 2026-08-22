#include <QtTest>

#include "stationconnectdisplaymode.h"

class TestStationConnectDisplayMode : public QObject
{
    Q_OBJECT

private slots:
    void usesDetectedResolutionInAutoMode();
    void usesConfiguredResolutionInOverrideMode();
    void clampsToQualifiedMaximum();
    void preservesUltrawideAspectRatio();
    void preservesExactNativeMatchAboveQualifiedMaximum();
    void capsNonMatchingNativeResolution();
    void capsExplicitOverrideDespiteNativeMatch();
    void fallsBackWhenDetectionFails();
};

void TestStationConnectDisplayMode::usesDetectedResolutionInAutoMode()
{
    QCOMPARE(StationConnectDisplayMode::resolve(true,
                                                QSize(2560, 1600),
                                                QSize(1920, 1080)),
             QSize(2560, 1600));
}

void TestStationConnectDisplayMode::usesConfiguredResolutionInOverrideMode()
{
    QCOMPARE(StationConnectDisplayMode::resolve(false,
                                                QSize(3840, 2160),
                                                QSize(1920, 1200)),
             QSize(1920, 1200));
}

void TestStationConnectDisplayMode::clampsToQualifiedMaximum()
{
    QCOMPARE(StationConnectDisplayMode::resolve(true,
                                                QSize(5120, 2880),
                                                QSize()),
             QSize(3840, 2160));
}

void TestStationConnectDisplayMode::preservesUltrawideAspectRatio()
{
    QCOMPARE(StationConnectDisplayMode::resolve(true,
                                                QSize(5120, 2160),
                                                QSize()),
             QSize(3840, 1620));
}

void TestStationConnectDisplayMode::preservesExactNativeMatchAboveQualifiedMaximum()
{
    QCOMPARE(StationConnectDisplayMode::resolve(true,
                                                QSize(5120, 2160),
                                                QSize(),
                                                QSize(5120, 2160)),
             QSize(5120, 2160));
}

void TestStationConnectDisplayMode::capsNonMatchingNativeResolution()
{
    QCOMPARE(StationConnectDisplayMode::resolve(true,
                                                QSize(5120, 2160),
                                                QSize(),
                                                QSize(5120, 2880)),
             QSize(3840, 1620));
}

void TestStationConnectDisplayMode::capsExplicitOverrideDespiteNativeMatch()
{
    QCOMPARE(StationConnectDisplayMode::resolve(false,
                                                QSize(5120, 2160),
                                                QSize(5120, 2160),
                                                QSize(5120, 2160)),
             QSize(3840, 1620));
}

void TestStationConnectDisplayMode::fallsBackWhenDetectionFails()
{
    QCOMPARE(StationConnectDisplayMode::resolve(true, QSize(), QSize(1280, 720)),
             QSize(3840, 2160));
}

QTEST_APPLESS_MAIN(TestStationConnectDisplayMode)
#include "test_stationconnectdisplaymode.moc"
