#include <QtTest>
#include <QGuiApplication>
#include <QScreen>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <fontconfig/fontconfig.h>
#include <memory>
#include "../../app/backend/systemproperties.h"
#include "../../app/backend/clientdisplays.h"
#include "../../app/backend/outputtopology.h"

// These platform predicates need no session or SDL dependency. In particular,
// this test links no SDL library: launcher display enumeration must be Qt-only.
namespace WMUtils {
bool isRunningDesktopEnvironment() { return true; }
bool isRunningWayland() { return QGuiApplication::platformName() == "wayland"; }
}

class QtSystemPropertiesTest : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        qmlRegisterSingletonType<SystemProperties>("SystemProperties", 1, 0, "SystemProperties",
            [](QQmlEngine*, QJSEngine*) -> QObject* { return new SystemProperties; });
    }

    void bookmarkChoicesPersistWithoutChangingOtherSettings()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QSettings settings(directory.filePath("bookmarks.ini"), QSettings::IniFormat);
        settings.beginGroup("bookmark");
        settings.setValue("scaling", "native");
        QCOMPARE(ClientDisplays::load(settings), QStringLiteral("span"));
        for (const auto& value : {QStringLiteral("current"), QStringLiteral("span"), QStringLiteral("screen:DP-1")}) {
            QVERIFY(ClientDisplays::validChoice(value));
            ClientDisplays::save(settings, value);
            settings.sync();
            QSettings reopened(settings.fileName(), QSettings::IniFormat);
            reopened.beginGroup("bookmark");
            QCOMPARE(ClientDisplays::load(reopened), value);
            QCOMPARE(reopened.value("scaling").toString(), QStringLiteral("native"));
        }
        for (const auto& invalid : {QString(), QStringLiteral("screen:"), QStringLiteral("other"), QStringLiteral("screen:DP\n1")}) {
            QVERIFY(!ClientDisplays::validChoice(invalid));
            settings.setValue("plank-client-displays", invalid);
            QCOMPARE(ClientDisplays::load(settings), QStringLiteral("current"));
        }
    }

    void selectedDisplaysDriveMatchClient_data()
    {
        QTest::addColumn<QString>("choice");
        QTest::addColumn<QString>("target");
        QTest::addColumn<int>("count");
        QTest::addColumn<bool>("fractional");
        QTest::newRow("current-right") << QStringLiteral("current") << QStringLiteral("HDMI-1") << 1 << false;
        QTest::newRow("explicit-left") << QStringLiteral("screen:DP-1") << QStringLiteral("DP-1") << 1 << false;
        QTest::newRow("explicit-right-125-percent") << QStringLiteral("screen:HDMI-1") << QStringLiteral("HDMI-1") << 1 << true;
        QTest::newRow("span") << QStringLiteral("span") << QStringLiteral("HDMI-1") << 2 << false;
        QTest::newRow("span-125-percent") << QStringLiteral("span") << QStringLiteral("HDMI-1") << 2 << true;
    }

    void selectedDisplaysDriveMatchClient()
    {
        QFETCH(QString, choice); QFETCH(QString, target); QFETCH(int, count); QFETCH(bool, fractional);
        const QSize logical = fractional ? QSize(2048, 1728) : QSize(2560, 2160);
        // Identical panels, reversed enumeration and a non-primary current monitor.
        const QVector<ClientDisplays::Output> outputs{
            {"HDMI-1", "Panel", QRect(QPoint(logical.width(), 0), logical), {2560, 2160}, false},
            {"DP-1", "Panel", QRect(QPoint(), logical), {2560, 2160}, true}};
        QVERIFY(ClientDisplays::canSpan(outputs));
        const auto selected = ClientDisplays::resolve(choice, outputs, "HDMI-1");
        QCOMPARE(selected.target, target);
        QCOMPARE(selected.names.size(), count);
        QVERIFY(selected.notice.isEmpty());
        QVector<NvClientDisplay> displays;
        for (const auto& output : outputs) if (selected.names.contains(output.name))
            displays.append({output.bounds, output.pixels, {}, output.name == selected.target});
        QString layout, error;
        QStringList modes;
        int primary = -1;
        QVERIFY2(NvOutputTopology::resolveClientDisplayLayout(displays, layout, modes, &error, &primary), qPrintable(error));
        QCOMPARE(modes.size(), count);
        for (const auto& mode : modes) QCOMPARE(mode, QStringLiteral("2560x2160"));
        QCOMPARE(primary, count == 1 ? 0 : 1);
        QCOMPARE(NvOutputTopology::resolveMacClientDisplayMode(displays),
                 count == 1 ? QStringLiteral("2560x2160") : QStringLiteral("5120x2160"));
    }

    void missingAndUnsupportedDisplayLayouts()
    {
        QVector<ClientDisplays::Output> outputs{{"DP-1", "Panel", {0, 0, 2560, 2160}, {2560, 2160}, true}};
        const auto missing = ClientDisplays::resolve("screen:HDMI-1", outputs, "not-present");
        QCOMPARE(missing.names, QStringList{"DP-1"});
        QVERIFY(!missing.notice.isEmpty());
        QVERIFY(!ClientDisplays::resolve("span", outputs, "DP-1").notice.isEmpty());
        outputs.append({"HDMI-1", "Panel", {0, 2160, 2560, 2160}, {2560, 2160}, false});
        QVERIFY(!ClientDisplays::canSpan(outputs));
        QCOMPARE(ClientDisplays::resolve("span", outputs, "HDMI-1").names, QStringList{"HDMI-1"});
        outputs[1].bounds.moveTopLeft({2560, 0});
        outputs.append({"DP-2", "Panel", {5120, 0, 2560, 2160}, {2560, 2160}, false});
        QVERIFY(!ClientDisplays::canSpan(outputs));
        QCOMPARE(ClientDisplays::resolve("screen:DP-2", outputs, "DP-1").names, QStringList{"DP-2"});
        QVERIFY(ClientDisplays::resolve("current", {}, {}).names.isEmpty());
    }

    void bookmarkDisplaySelectorLoadsAndRetainsUnavailableChoice()
    {
        QQmlEngine engine;
        QQmlComponent component(&engine, QUrl("qrc:/selection/PlankClientDisplayBox.qml"));
        std::unique_ptr<QObject> selector(component.create());
        QVERIFY2(selector, qPrintable(component.errorString()));
        QCOMPARE(selector->property("selection").toString(), QStringLiteral("current"));
        auto* choice = selector->findChild<QObject*>("clientDisplayChoice");
        QVERIFY(choice);
        QCOMPARE(choice->property("currentValue").toString(), QStringLiteral("current"));
        selector->setProperty("selection", "screen:absent-test-output");
        QCOMPARE(choice->property("currentValue").toString(), QStringLiteral("screen:absent-test-output"));
        QVERIFY(choice->property("displayText").toString().contains("Unavailable"));
        selector->setProperty("selection", "span");
        QCOMPARE(choice->property("currentValue").toString(), QStringLiteral("span"));
    }

    void decoderQualificationIsDeferred()
    {
        SystemProperties properties;
        QVERIFY(!properties.property("hardwareCapabilityKnown").toBool());
        QVERIFY(!properties.property("rendererAlwaysFullScreen").toBool());
    }

    void settingsDescribeAutomaticRecovery()
    {
        SystemProperties properties;
        QVERIFY(properties.property("automaticSessionRecovery").toBool());
    }

    void nativeDisplayGeometryAndRepeatedRefresh()
    {
        SystemProperties properties;
        const auto screens = QGuiApplication::screens();
        QVERIFY(!screens.isEmpty());
        for (int refresh = 0; refresh < 3; ++refresh) {
            properties.refreshDisplays();
            for (int i = 0; i < screens.size(); ++i) {
                const auto screen = screens[i];
                QCOMPARE(properties.getNativeResolution(i).size(),
                         screen->geometry().size() * screen->devicePixelRatio());
                QCOMPARE(properties.getSafeAreaResolution(i).size(),
                         screen->availableGeometry().size() * screen->devicePixelRatio());
                QVERIFY(properties.getRefreshRate(i) > 0);
            }
            QVERIFY(properties.getNativeResolution(screens.size()).isNull());
            QVERIFY(properties.getSafeAreaResolution(screens.size()).isNull());
            QCOMPARE(properties.getRefreshRate(screens.size()), 0);
        }
    }
};
int main(int argc, char** argv)
{
    const auto cleanup = qScopeGuard([] { FcFini(); });
    qputenv("QT_QUICK_CONTROLS_MATERIAL_THEME", "Dark");
    qputenv("QT_QUICK_CONTROLS_MATERIAL_VARIANT", "Dense");
    QQuickStyle::setStyle(QStringLiteral("Material"));
    QGuiApplication app(argc, argv);
    QtSystemPropertiesTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_qtsystemproperties.moc"
