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
    void fitsClosestSupportedModeWithoutUpscale();
    void keepsStreamDimensionsEven();
    void streamsArrangementCanvasOneToOne();
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

void TestPlankDisplayMode::fitsClosestSupportedModeWithoutUpscale()
{
    // Match client on a MacBook Pro 14": the host runs the closest supported
    // 2560x1600 and the stream fits the match target, never enlarged.
    // Default Retina: the target is the 3024x1964 panel.
    QCOMPARE(PlankDisplayMode::resolve(QSize(3024, 1964), QSize(2560, 1600)), QSize(2560, 1600));
    // 1x "1920x1200" desktop: the target is the desktop, not the panel, so
    // the stream is no larger than what is presented.
    QCOMPARE(PlankDisplayMode::resolve(QSize(1920, 1200), QSize(2560, 1600)), QSize(1920, 1200));
    // Nothing fits a small 1x desktop: 1920x1200 is scaled down, aspect kept.
    QCOMPARE(PlankDisplayMode::resolve(QSize(1512, 982), QSize(1920, 1200)), QSize(1512, 944));
    // Two fitted displays: 2560x1600 + 3840x2160 into the 3024+3840 target.
    QCOMPARE(PlankDisplayMode::resolve(QSize(6864, 2160), QSize(6400, 2160)), QSize(6400, 2160));
}

void TestPlankDisplayMode::keepsStreamDimensionsEven()
{
    for (const QSize target : {QSize(3024, 1964), QSize(1920, 1200), QSize(1511, 983),
                               QSize(1366, 768), QSize(2940, 1912)}) {
        for (const QSize canvas : {QSize(2560, 1600), QSize(1920, 1200), QSize(3840, 2160),
                                   QSize(5120, 2160)}) {
            const QSize stream = PlankDisplayMode::resolve(target, canvas);
            QVERIFY(stream.width() % 2 == 0 && stream.height() % 2 == 0);
            QVERIFY(stream.width() <= canvas.width() && stream.height() <= canvas.height());
            QVERIFY(stream.width() <= target.width() && stream.height() <= target.height());
        }
    }
}

void TestPlankDisplayMode::streamsArrangementCanvasOneToOne()
{
    // A display arrangement streams the workstation desktop as it is, far
    // past the legacy 3840x2160 ceiling.
    QCOMPARE(PlankDisplayMode::resolveClient(QSize(6864, 2160), QSize(8192, 8192)), QSize(6864, 2160));
    QCOMPARE(PlankDisplayMode::resolveClient(QSize(7680, 4320), QSize()), QSize(7680, 4320));
    QCOMPARE(PlankDisplayMode::qualifiedMaximum(), QSize(3840, 2160));
    // Never scaled silently: too large or odd is an error with a reason.
    QString error;
    QVERIFY(!PlankDisplayMode::resolveClient(QSize(9600, 4320), QSize(8192, 8192), &error).isValid());
    QVERIFY(error.contains(QStringLiteral("9600x4320")));
    QVERIFY(!PlankDisplayMode::resolveClient(QSize(6864, 2160), QSize(4096, 4096), &error).isValid());
    QVERIFY(!PlankDisplayMode::resolveClient(QSize(3025, 1890), QSize(), &error).isValid());
    QVERIFY(!PlankDisplayMode::resolveClient(QSize(), QSize(), &error).isValid());
    QCOMPARE(PlankDisplayMode::resolveClient(QSize(3024, 1890), QSize(3024, 1890), &error), QSize(3024, 1890));
    QVERIFY(error.isEmpty());
}

QTEST_APPLESS_MAIN(TestPlankDisplayMode)
#include "test_plankdisplaymode.moc"
