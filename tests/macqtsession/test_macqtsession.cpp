#include "sessionwindow.h"
#include "metalsessionshell.h"
#include "remotekeymap.h"
#include "sessionshortcut.h"
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
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
        QCOMPARE(qtRemoteKey(1000).vk, 0);
        const auto mods = qtPhysicalModifiers(Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(mods, Qt::MetaModifier | Qt::ShiftModifier);
        QCOMPARE(qtSessionShortcut(12, Qt::Key_unknown,
            Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier), int(Qt::Key_Q));
        // Ordinary Command-Q belongs to the remote app, not local disconnect.
        QCOMPARE(qtSessionShortcut(12, Qt::Key_Q, Qt::MetaModifier), 0);
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

        scene.resize(1100, 700);
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
