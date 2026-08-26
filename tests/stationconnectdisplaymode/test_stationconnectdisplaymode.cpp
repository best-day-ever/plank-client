#include <QtTest>

#include "stationconnectdisplaymode.h"

class TestStationConnectDisplayMode : public QObject
{
    Q_OBJECT

private slots:
    void usesDetectedClientResolution();
    void preservesDetectedResolutionAboveFallback();
    void scalesHostCanvasDirectlyToClient();
    void preservesExactNativeMatch();
    void avoidsHostUpscale();
    void fallsBackWhenDetectionFails();
};

void TestStationConnectDisplayMode::usesDetectedClientResolution()
{
    QCOMPARE(StationConnectDisplayMode::resolve(QSize(2560, 1600)),
             QSize(2560, 1600));
}

void TestStationConnectDisplayMode::preservesDetectedResolutionAboveFallback()
{
    QCOMPARE(StationConnectDisplayMode::resolve(QSize(5120, 2880)),
             QSize(5120, 2880));
}

void TestStationConnectDisplayMode::scalesHostCanvasDirectlyToClient()
{
    QCOMPARE(StationConnectDisplayMode::resolve(QSize(4096, 1728),
                                                QSize(5120, 2160)),
             QSize(4096, 1728));
}

void TestStationConnectDisplayMode::preservesExactNativeMatch()
{
    QCOMPARE(StationConnectDisplayMode::resolve(QSize(5120, 2160),
                                                QSize(5120, 2160)),
             QSize(5120, 2160));
}

void TestStationConnectDisplayMode::avoidsHostUpscale()
{
    QCOMPARE(StationConnectDisplayMode::resolve(QSize(3840, 2160),
                                                QSize(1920, 1080)),
             QSize(1920, 1080));
}

void TestStationConnectDisplayMode::fallsBackWhenDetectionFails()
{
    QCOMPARE(StationConnectDisplayMode::resolve(QSize()),
             QSize(3840, 2160));
}

QTEST_APPLESS_MAIN(TestStationConnectDisplayMode)
#include "test_stationconnectdisplaymode.moc"
