#include "../../app/backend/collaboration.h"
#include "../../app/streaming/plankpresentation.h"
#include <QSignalSpy>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QtTest>

class CollaborationTest : public QObject
{
    Q_OBJECT
    const QString id = QString(64, QLatin1Char('a'));
    QJsonObject approved(QString role = QStringLiteral("view")) const {
        return {{"request_id", id}, {"state", role}, {"grant", QString(64, QLatin1Char('b'))},
                {"capture", QJsonObject{{"width", 5760}, {"height", 2160}, {"fps", 60}}},
                {"topology", QJsonObject{{"version", 1}}}};
    }
private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }
    void grantsStayOutOfDiagnostics() {
        const QUrl launch("https://host.example:47984/launch?appid=1&plankCollaboration=" + id);
        const auto safe = PlankCollaboration::diagnosticUrl(launch);
        QVERIFY(!safe.toString().contains(id));
        QCOMPARE(QUrlQuery(safe).queryItemValue("appid"), QString("1"));
        const QUrl status("https://host.example:47984/plank/collaboration/status?request_id=" + id);
        QVERIFY(!PlankCollaboration::diagnosticUrl(status).toString().contains(id));
    }
    void pendingCannotGrantControl() {
        auto object = approved(QStringLiteral("pending"));
        const auto reply = PlankCollaboration::parse(object, id);
        QCOMPARE(reply.state, PlankCollaboration::Pending);
        QCOMPARE(reply.permission, PlankCollaboration::View);
        QVERIFY(reply.grant.isEmpty());
    }
    void consentBindsRequest() {
        QCOMPARE(PlankCollaboration::parse(approved(), QString(64, QLatin1Char('c'))).state,
                 PlankCollaboration::Invalid);
        const auto reply = PlankCollaboration::parse(approved(), id);
        QCOMPARE(reply.state, PlankCollaboration::Approved);
        QCOMPARE(reply.size, QSize(5760, 2160));
        QCOMPARE(reply.permission, PlankCollaboration::View);
        QCOMPARE(PlankCollaboration::parse(approved("control"), id).permission, PlankCollaboration::Control);
    }
    void malformedApprovalFailsClosed() {
        for (const auto field : {"grant", "capture", "topology"}) {
            auto object = approved(); object.remove(QLatin1String(field));
            QCOMPARE(PlankCollaboration::parse(object, id).state, PlankCollaboration::Invalid);
        }
        auto object = approved(); object["state"] = "owner";
        QCOMPARE(PlankCollaboration::parse(object, id).state, PlankCollaboration::Invalid);
        object = approved(); object["request_id"] = "unsafe\nrequest";
        QCOMPARE(PlankCollaboration::parse(object).state, PlankCollaboration::Invalid);
    }
    void terminalDecisionsNeverAuthorize() {
        for (const auto state : {"denied", "revoked", "expired"}) {
            auto object = approved(QLatin1String(state));
            const auto reply = PlankCollaboration::parse(object, id);
            QVERIFY(reply.state != PlankCollaboration::Approved);
            QVERIFY(reply.grant.isEmpty());
        }
    }
    void livePermissionChangesFailClosed() {
        unsigned char message[4] {};
        QCOMPARE(PlankCollaboration::wirePermission(message, 4), 0);
        for (int permission = 1; permission <= 2; ++permission) {
            message[3] = permission;
            QCOMPARE(PlankCollaboration::wirePermission(message, 4), permission);
            QCOMPARE(PlankCollaboration::allowsInput(
                static_cast<PlankCollaboration::Permission>(permission), 5), permission == 2);
        }
        message[3] = 3;
        QCOMPARE(PlankCollaboration::wirePermission(message, 4), -1);
        message[3] = 1; message[0] = 1;
        QCOMPARE(PlankCollaboration::wirePermission(message, 4), -1);
        QCOMPARE(PlankCollaboration::wirePermission(message, 3), -1);
        QCOMPARE(PlankCollaboration::wirePermission(nullptr, 4), -1);
    }
    void inputPermissions() {
        for (unsigned type = 1; type <= 255; ++type) {
            QVERIFY(!PlankCollaboration::allowsInput(PlankCollaboration::View, type));
            QCOMPARE(PlankCollaboration::allowsInput(PlankCollaboration::Control, type), type <= 5);
            QVERIFY(PlankCollaboration::allowsInput(PlankCollaboration::Owner, type));
        }
    }
    void ownerArrangementScalesLocally() {
        const QVector<QRect> owner{{-2560, -240, 2560, 1440}, {0, 0, 3840, 2160}};
        const auto windows = PlankCollaboration::windowPlacement(owner, QRect(100, 50, 1600, 1000));
        QCOMPARE(windows.size(), 2);
        QCOMPARE(windows[0].width(), 512);
        QCOMPARE(windows[1].width(), 768);
        QCOMPARE(windows[1].x() - windows[0].x(), 512);
        QCOMPARE(windows[1].y() - windows[0].y(), 48);
        const auto small = PlankCollaboration::windowPlacement(owner, QRect(0, 0, 800, 600));
        QCOMPARE(small[0].width(), 256);
        QCOMPARE(small[1].x() - small[0].x(), 256);
        QCOMPARE(owner[0], QRect(-2560, -240, 2560, 1440));
        QVERIFY(PlankCollaboration::windowPlacement({}, QRect(0, 0, 800, 600)).isEmpty());
    }
    void packedOwnerOutputMapsAfterLocalResize() {
        const QSize capture(8192, 4320);
        PlankPresentationOutput output;
        output.desktopRect = QRect(7680, 0, 3840, 2160);
        output.sourceRect = PlankPresentation::sourceRectInStream(QRect(0, 2160, 3840, 2160), capture, capture);
        for (const QSize window : {QSize(768, 432), QSize(1000, 700)}) {
            QPointF desktop;
            QVERIFY(PlankPresentation::mapWindowPointToDesktop(
                QPointF(window.width() / 2.0, window.height() / 2.0), window,
                QSize(window.width() * 2, window.height() * 2), output, capture, desktop, false));
            QVERIFY(qAbs(desktop.x() - 9600) <= 1.0);
            QVERIFY(qAbs(desktop.y() - 1080) <= 1.0);
        }
        QCOMPARE(output.sourceRect, QRectF(0, 2160, 3840, 2160));
        QCOMPARE(output.desktopRect, QRect(7680, 0, 3840, 2160));
    }
    void connectedDesktopCanRequestJoin() {
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick 2.15
            import QtQuick.Controls 2.15
            import "qrc:/gui"
            ApplicationWindow {
                width: 900; height: 600; visible: true
                Item { id: stackView }
                CollaborationDialog { objectName: "collaboration" }
            })", QUrl("qrc:/collaboration-test.qml"));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));
        QObject* dialog = window->findChild<QObject*>("collaboration");
        QVERIFY(dialog);
        auto* quickWindow = qobject_cast<QQuickWindow*>(window.data());
        QVERIFY(quickWindow && QTest::qWaitForWindowExposed(quickWindow));
        dialog->setProperty("text", "The desktop owner is using this workstation. Request permission to join their desktop for remote support? They can allow viewing or interaction. Their display layout stays unchanged.");
        QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        auto* explanation = dialog->findChild<QObject*>("collaborationExplanation");
        QVERIFY(explanation);
        QVERIFY(explanation->property("contentWidth").toDouble() <= explanation->property("width").toDouble() + 1);
        const QString screenshot = qEnvironmentVariable("PLANK_COLLABORATION_SCREENSHOT");
        if (!screenshot.isEmpty()) QVERIFY(quickWindow->grabWindow().save(screenshot));
        QVERIFY(!dialog->property("signOutAvailable").toBool());
        auto *join = dialog->findChild<QObject*>("requestCollaboration");
        auto *signOut = dialog->findChild<QObject*>("signOutDesktop");
        QVERIFY(join); QVERIFY(signOut);
        QVERIFY(!signOut->property("visible").toBool());
        QSignalSpy chosen(dialog, SIGNAL(chosen(int)));
        QVERIFY(QMetaObject::invokeMethod(join, "clicked"));
        QCOMPARE(chosen.size(), 1);
        QCOMPARE(chosen[0][0].toInt(), 2);
        QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        auto* cancel = dialog->findChild<QObject*>("cancelCollaboration");
        QVERIFY(cancel);
        QVERIFY(QMetaObject::invokeMethod(cancel, "clicked"));
        QCOMPARE(chosen.size(), 2);
        QCOMPARE(chosen[1][0].toInt(), -1);
    }
};
QTEST_MAIN(CollaborationTest)
#include "test_collaboration.moc"
