#include "sessionwindow.h"
#include "metalsessionshell.h"
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
