#include "sessionwindow.h"
#include "metalsessionshell.h"
#include "remotekeymap.h"
#include "sessionshortcut.h"
#include <QDir>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QTabletEvent>
#include <QtTest>

namespace {
class Control final : public QQuickRenderControl
{
public:
    explicit Control(QWindow* host) : m_Host(host) {}
    QWindow* renderWindow(QPoint* offset) override
    {
        if (offset) *offset = {};
        return m_Host;
    }
private:
    QWindow* m_Host;
};
void pointer(QQuickWindow& window, QEvent::Type type, QPointF point, Qt::MouseButton button)
{
    QMouseEvent event(type, point, point, point, button,
        type == QEvent::MouseButtonRelease ? Qt::NoButton : button, Qt::NoModifier);
    QCoreApplication::sendEvent(&window, &event);
}
}

class MacQtSessionTest final : public QObject
{
    Q_OBJECT
private slots:
    void physicalMacKeysAndShortcuts()
    {
        QCOMPARE(qtRemoteKey(0).vk, 0x41); // A has a valid zero Cocoa code.
        QCOMPARE(qtRemoteKey(56).vk, 0xa0);
        QCOMPARE(qtRemoteKey(60).vk, 0xa1);
        QCOMPARE(qtRemoteKey(55).vk, 0x5b);
        QCOMPARE(qtRemoteKey(59).vk, 0xa2);
        QCOMPARE(qtRemoteKey(76).vk, 0x0d);
        QCOMPARE(qtRemoteKey(52).vk, 0x0d); // Portable keyboard's Enter alias.
        QCOMPARE(qtRemoteKey(102).vk, 0x1d); // JIS Eisu and Kana.
        QCOMPARE(qtRemoteKey(104).vk, 0x1c);
        // Retain the existing Mac keyboard protocol, including extended PC
        // keys on external keyboards and the application/context-menu key.
        QCOMPARE(qtRemoteKey(105).vk, 0x2c);
        QCOMPARE(qtRemoteKey(107).vk, 0x91);
        QCOMPARE(qtRemoteKey(113).vk, 0x13);
        QCOMPARE(qtRemoteKey(110).vk, 0x5d);
        QCOMPARE(qtRemoteKey(1000).vk, 0);
        const auto mods = qtPhysicalModifiers(Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(mods, Qt::MetaModifier | Qt::ShiftModifier);
        QCOMPARE(qtSessionShortcut(12, Qt::Key_unknown,
            Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier), int(Qt::Key_Q));
        // Ordinary Command-Q belongs to the remote app, not local disconnect.
        QCOMPARE(qtSessionShortcut(12, Qt::Key_Q, Qt::MetaModifier), 0);
        QCOMPARE(qtSessionShortcut(46, Qt::Key_M,
            Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier), int(Qt::Key_M));
    }
    void immersivePointerRoutesButtonsAndRejectsPreReleaseMotion()
    {
        QWindow host; host.resize(1280, 900);
        Control control(&host); QtSessionWindow scene(&host, &control);
        scene.resize(host.size()); scene.setRemoteInputEnabled(true);
        scene.setPresentation({QSize(1280,900), QRectF(0,0,1280,900),
            QRectF(0,0,1280,900), QSize(1280,900), host.size()});
        scene.setImmersiveMouse(true, true); scene.setPointerCaptured(true);
        QSignalSpy buttons(&scene, &QtSessionWindow::remoteButton);
        pointer(scene, QEvent::MouseButtonPress, {5,5}, Qt::LeftButton);
        pointer(scene, QEvent::MouseButtonRelease, {5,5}, Qt::LeftButton);
        QCOMPARE(buttons.size(), 2); // Frozen cursor over the toolbar still controls the Host.
        scene.setPointerCaptured(false);
        scene.discardPointerMotionBefore(100);
        QSignalSpy motion(&scene, &QtSessionWindow::remoteMotion);
        QMouseEvent old(QEvent::MouseMove, {400,400}, {400,400}, {}, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        old.setTimestamp(99); QCoreApplication::sendEvent(&scene,&old);
        QCOMPARE(motion.size(),0);
        QMouseEvent current(QEvent::MouseMove, {410,410}, {410,410}, {}, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        current.setTimestamp(101); QCoreApplication::sendEvent(&scene,&current);
        QCOMPARE(motion.size(),1);
    }
    void redirectedSceneKeepsControlsAndRemoteInputSeparate()
    {
        QWindow host;
        host.resize(1440, 900);
        Control control(&host);
        QtSessionWindow scene(&host, &control);
        scene.resize(host.size());
        scene.setLiveSession(true);
        scene.setPinned(true);
        scene.setRemoteInputEnabled(true);
        QCOMPARE(scene.status(), QQmlComponent::Ready);
        QVERIFY(scene.rootObject());
        QCOMPARE(scene.rootObject()->size(), QSizeF(host.size()));
        QCOMPARE(QQuickRenderControl::renderWindowFor(&scene), &host);
        scene.setPresentation({QSize(2880, 1800), QRectF(0, 0, 2880, 1800),
            QRectF(0, 0, 2880, 1800), QSize(2880, 1800), host.size()});
        QImage image(host.size(), QImage::Format_ARGB32_Premultiplied);
        scene.setRenderTarget(QQuickRenderTarget::fromPaintDevice(&image));
        control.polishItems(); control.sync(); control.render();
        const auto captures = qEnvironmentVariable("PLANK_QT_SESSION_CAPTURE_DIR");
        if (!captures.isEmpty()) {
            QVERIFY(QDir().mkpath(captures));
            QVERIFY(image.save(captures + QStringLiteral("/macos-qt-toolbar.png")));
        }

        QSignalSpy buttons(&scene, &QtSessionWindow::remoteButton);
        pointer(scene, QEvent::MouseButtonPress, {720, 450}, Qt::LeftButton);
        pointer(scene, QEvent::MouseButtonRelease, {720, 450}, Qt::LeftButton);
        QCOMPARE(buttons.count(), 2);
        QCOMPARE(buttons.first().at(2).toPointF(), QPointF(1440, 900));

        auto* fullscreen = scene.rootObject()->findChild<QQuickItem*>("fullscreenButton");
        QVERIFY(fullscreen);
        QSignalSpy toggles(&scene, &QtSessionWindow::fullscreenRequested);
        const auto point = fullscreen->mapToScene(fullscreen->boundingRect().center());
        pointer(scene, QEvent::MouseButtonPress, point, Qt::LeftButton);
        pointer(scene, QEvent::MouseButtonRelease, point, Qt::LeftButton);
        QCOMPARE(toggles.count(), 1);
        QCOMPARE(buttons.count(), 2);

        auto* newConnection = scene.rootObject()->findChild<QQuickItem*>("newConnectionButton");
        QVERIFY(newConnection);
        QSignalSpy newWindows(&scene, &QtSessionWindow::newConnectionRequested);
        QSignalSpy disconnects(&scene, &QtSessionWindow::disconnectRequested);
        const auto newPoint = newConnection->mapToScene(newConnection->boundingRect().center());
        pointer(scene, QEvent::MouseButtonPress, newPoint, Qt::LeftButton);
        pointer(scene, QEvent::MouseButtonRelease, newPoint, Qt::LeftButton);
        QCOMPARE(newWindows.count(), 1);
        QCOMPARE(disconnects.count(), 0);
        QCOMPARE(buttons.count(), 2); // New connection is a local control.

        const QPointingDevice pen(QStringLiteral("test pen"), 1, QInputDevice::DeviceType::Stylus,
            QPointingDevice::PointerType::Pen, QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1, 3);
        const auto tablet = [&](QEvent::Type type, QPointF position, bool down) {
            QTabletEvent event(type, &pen, position, position, down ? .7 : 0, 10, 20, 0, 0, 0,
                Qt::NoModifier, Qt::LeftButton, down ? Qt::LeftButton : Qt::NoButton);
            QCoreApplication::sendEvent(&scene, &event);
        };
        QSignalSpy strokes(&scene, &QtSessionWindow::remotePen);
        QSignalSpy cancels(&scene, &QtSessionWindow::remotePenCancelled);
        tablet(QEvent::TabletPress, {720,450}, true);
        tablet(QEvent::TabletRelease, {1600,1000}, false);
        QCOMPARE(strokes.count(), 2);
        QCOMPARE(strokes.last().at(6).toInt(), int(Qt::NoButton));
        tablet(QEvent::TabletPress, {720,450}, true);
        tablet(QEvent::TabletRelease, point, false); // release over the toolbar
        QCOMPARE(strokes.count(), 4);
        QCOMPARE(strokes.last().at(6).toInt(), int(Qt::NoButton));
        tablet(QEvent::TabletPress, {720,450}, true);

        scene.resize(1100, 700);
        QVERIFY(!cancels.isEmpty());
        QCOMPARE(scene.rootObject()->size(), QSizeF(1100, 700));
        QVERIFY(!scene.presentation().valid()); // Input awaits matching new geometry.
        scene.setWaiting(true);
        QCOMPARE(host.cursor().shape(), Qt::ArrowCursor);
        control.invalidate();
    }
    void windowedSizeUsesLogicalAvailableArea()
    {
        QCOMPARE(QtMetalSessionShell::defaultWindowedSize({1440, 900}, {5120, 2160}), QSize(1152, 486));
        QCOMPARE(QtMetalSessionShell::defaultWindowedSize({1440, 900}, {}), QSize(1152, 720));
        QVERIFY(QtMetalSessionShell::defaultWindowedSize({}, {3840, 2160}).isEmpty());
    }
};

QTEST_MAIN(MacQtSessionTest)
#include "test_macqtsession.moc"
