#include <QtTest>

#include "clientdisplayprobe.h"
#include "outputtopology.h"

// Display setup: monitor identity, profiles and the layout planner.

namespace {

NvClientDisplay monitor(const QString& key, const QRect& bounds, const QSize& pixels,
                        const QString& name = QString(), bool builtIn = false)
{
    NvClientDisplay display {bounds, pixels, pixels};
    display.key = key;
    display.name = name;
    display.builtIn = builtIn;
    return display;
}

}

class TestDisplayPlanner : public QObject
{
    Q_OBJECT

private slots:
    void monitorKeysPreferTheUuid();
    void fingerprintIgnoresSizeAndPosition();
    void duplicateMonitorsGetDistinctKeys();
    void labelsTheSetLeftToRight();
    void macMatchUsesTheFullscreenViewport();
};

void TestDisplayPlanner::monitorKeysPreferTheUuid()
{
    QCOMPARE(ClientDisplayProbe::monitorKey(QStringLiteral(" 1e2d3c4b-0000-4000-8000-00000000abcd "), 0x610, 0xa050, 7,
                                            true, QSize(3024, 1964)),
             QStringLiteral("uuid:1E2D3C4B-0000-4000-8000-00000000ABCD"));
    QCOMPARE(ClientDisplayProbe::monitorKey(QString(), 0x1e6d, 0x5b11, 0x1234, false, QSize(3840, 2160)),
             QStringLiteral("edid:1e6d-5b11-1234"));
    QCOMPARE(ClientDisplayProbe::monitorKey(QString(), 0, 0, 0, true, QSize(3024, 1964)),
             QStringLiteral("builtin:3024x1964"));
    QCOMPARE(ClientDisplayProbe::monitorKey(QString(), 0, 0, 0, false, QSize(1920, 1080)),
             QStringLiteral("panel:1920x1080"));
}

void TestDisplayPlanner::fingerprintIgnoresSizeAndPosition()
{
    const NvClientDisplay laptop = monitor(QStringLiteral("uuid:A"), QRect(0, 0, 1512, 982), QSize(3024, 1964));
    const NvClientDisplay external = monitor(QStringLiteral("uuid:B"), QRect(1512, 0, 2560, 1440), QSize(5120, 2880));
    const QString docked = ClientDisplayProbe::fingerprint({laptop, external});
    QCOMPARE(docked.size(), 20);
    // The same monitors placed and scaled differently, in any order.
    NvClientDisplay movedLaptop = laptop;
    movedLaptop.bounds = QRect(2560, 400, 1800, 1169);
    movedLaptop.backingSize = QSize(3600, 2338);
    NvClientDisplay movedExternal = external;
    movedExternal.bounds = QRect(0, 0, 3840, 2160);
    QCOMPARE(ClientDisplayProbe::fingerprint({movedExternal, movedLaptop}), docked);
    // A different set is a different fingerprint; no monitors, none at all.
    QVERIFY(ClientDisplayProbe::fingerprint({laptop}) != docked);
    QVERIFY(ClientDisplayProbe::fingerprint({}).isEmpty());
}

void TestDisplayPlanner::duplicateMonitorsGetDistinctKeys()
{
    QVector<NvClientDisplay> displays {
        monitor(QStringLiteral("edid:10ac-a0c4-0"), QRect(0, 0, 1920, 1080), QSize(1920, 1080)),
        monitor(QStringLiteral("edid:10ac-a0c4-0"), QRect(1920, 0, 1920, 1080), QSize(1920, 1080)),
        monitor(QString(), QRect(3840, 0, 1920, 1080), QSize(1920, 1080)),
    };
    ClientDisplayProbe::assignUniqueKeys(displays);
    QCOMPARE(displays.at(0).key, QStringLiteral("edid:10ac-a0c4-0"));
    QCOMPARE(displays.at(1).key, QStringLiteral("edid:10ac-a0c4-0#2"));
    QCOMPARE(displays.at(2).key, QStringLiteral("panel:1920x1080"));
    // Stable: assigning again changes nothing.
    const QVector<NvClientDisplay> again = displays;
    ClientDisplayProbe::assignUniqueKeys(displays);
    for (int index = 0; index < displays.size(); ++index) QCOMPARE(displays.at(index).key, again.at(index).key);
}

void TestDisplayPlanner::labelsTheSetLeftToRight()
{
    const NvClientDisplay laptop = monitor(QStringLiteral("uuid:A"), QRect(0, 0, 1512, 982), QSize(3024, 1964),
                                           QStringLiteral("Built-in Retina Display"), true);
    const NvClientDisplay external = monitor(QStringLiteral("uuid:B"), QRect(-2560, 0, 2560, 1440),
                                             QSize(5120, 2880), QStringLiteral("LG UltraFine"));
    QCOMPARE(ClientDisplayProbe::label({laptop, external}),
             QStringLiteral("LG UltraFine + Built-in Retina Display"));
    const NvClientDisplay unnamed = monitor(QStringLiteral("uuid:C"), QRect(1512, 0, 1920, 1080), QSize(1920, 1080));
    QCOMPARE(ClientDisplayProbe::label({laptop, unnamed}), QStringLiteral("Built-in Retina Display + Display"));
}

void TestDisplayPlanner::macMatchUsesTheFullscreenViewport()
{
    // 14" MacBook Pro at the default 1512x982 pt: 38 pt camera housing.
    NvClientDisplay notched {QRect(0, 0, 1512, 982), QSize(3024, 1964), QSize(3024, 1964), QSize(3024, 1888)};
    const NvClientDisplay external {QRect(1512, -200, 1920, 1080), QSize(1920, 1080), QSize(1920, 1080)};
    const QVector<NvClientDisplay> windowed = ClientDisplayProbe::macMatchDisplays({notched, external}, false);
    QCOMPARE(windowed.at(0).bounds, notched.bounds);
    QCOMPARE(windowed.at(0).backingSize, QSize(3024, 1964));
    const QVector<NvClientDisplay> fullscreen = ClientDisplayProbe::macMatchDisplays({notched, external}, true);
    QCOMPARE(fullscreen.at(0).bounds, QRect(0, 38, 1512, 944));
    QCOMPARE(fullscreen.at(0).backingSize, QSize(3024, 1888));
    QCOMPARE(fullscreen.at(0).nativeSize, QSize(3024, 1888));
    QCOMPARE(fullscreen.at(1).bounds, external.bounds);
    QCOMPARE(fullscreen.at(1).backingSize, QSize(1920, 1080));
    int scale = 0;
    QCOMPARE(NvOutputTopology::resolveMacClientDisplayMode(fullscreen, nullptr, &scale), QString());
    QCOMPARE(NvOutputTopology::resolveMacClientDisplayMode({fullscreen.at(0)}, nullptr, &scale),
             QStringLiteral("3024x1888"));
    QCOMPARE(scale, 2);
}

QTEST_GUILESS_MAIN(TestDisplayPlanner)
#include "test_displayplanner.moc"
