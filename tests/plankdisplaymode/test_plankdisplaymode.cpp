#include <QtTest>

#include "plankdisplaymode.h"

class TestPlankDisplayMode : public QObject
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

void TestPlankDisplayMode::usesDetectedClientResolution()
{
    QCOMPARE(PlankDisplayMode::resolve(QSize(2560, 1600)),
             QSize(2560, 1600));
}

void TestPlankDisplayMode::preservesDetectedResolutionAboveFallback()
{
    QCOMPARE(PlankDisplayMode::resolve(QSize(5120, 2880)),
             QSize(5120, 2880));
}

void TestPlankDisplayMode::scalesHostCanvasDirectlyToClient()
{
    QCOMPARE(PlankDisplayMode::resolve(QSize(4096, 1728),
                                                QSize(5120, 2160)),
             QSize(4096, 1728));
}

void TestPlankDisplayMode::preservesExactNativeMatch()
{
    QCOMPARE(PlankDisplayMode::resolve(QSize(5120, 2160),
                                                QSize(5120, 2160)),
             QSize(5120, 2160));
}

void TestPlankDisplayMode::avoidsHostUpscale()
{
    QCOMPARE(PlankDisplayMode::resolve(QSize(3840, 2160),
                                                QSize(1920, 1080)),
             QSize(1920, 1080));
}

void TestPlankDisplayMode::fallsBackWhenDetectionFails()
{
    QCOMPARE(PlankDisplayMode::resolve(QSize()),
             QSize(3840, 2160));
}

QTEST_APPLESS_MAIN(TestPlankDisplayMode)
#include "test_plankdisplaymode.moc"
