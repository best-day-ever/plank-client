#include <QtTest>

#include "stationconnectdisplaymode.h"

class TestStationConnectDisplayMode : public QObject
{
    Q_OBJECT

private slots:
    void usesDetectedClientResolution();
    void clampsToQualifiedMaximum();
    void preservesUltrawideAspectRatio();
    void preservesExactNativeMatchAboveQualifiedMaximum();
    void capsNonMatchingNativeResolution();
    void fallsBackWhenDetectionFails();
};

void TestStationConnectDisplayMode::usesDetectedClientResolution()
{
    QCOMPARE(StationConnectDisplayMode::resolve(QSize(2560, 1600)),
             QSize(2560, 1600));
}

void TestStationConnectDisplayMode::clampsToQualifiedMaximum()
{
    QCOMPARE(StationConnectDisplayMode::resolve(QSize(5120, 2880)),
             QSize(3840, 2160));
}

void TestStationConnectDisplayMode::preservesUltrawideAspectRatio()
{
    QCOMPARE(StationConnectDisplayMode::resolve(QSize(5120, 2160)),
             QSize(3840, 1620));
}

void TestStationConnectDisplayMode::preservesExactNativeMatchAboveQualifiedMaximum()
{
    QCOMPARE(StationConnectDisplayMode::resolve(QSize(5120, 2160),
                                                QSize(5120, 2160)),
             QSize(5120, 2160));
}

void TestStationConnectDisplayMode::capsNonMatchingNativeResolution()
{
    QCOMPARE(StationConnectDisplayMode::resolve(QSize(5120, 2160),
                                                QSize(5120, 2880)),
             QSize(3840, 1620));
}

void TestStationConnectDisplayMode::fallsBackWhenDetectionFails()
{
    QCOMPARE(StationConnectDisplayMode::resolve(QSize()),
             QSize(3840, 2160));
}

QTEST_APPLESS_MAIN(TestStationConnectDisplayMode)
#include "test_stationconnectdisplaymode.moc"
