#include <QtTest>
#include "desktopstage.h"

class TestDesktopStage : public QObject
{
    Q_OBJECT
private slots:
    void requiresAuthenticatedGreeter()
    {
        QJsonObject response {{"state", "authenticated"},
                              {"session_token", "synthetic-test-token"},
                              {"desktop_stage", "greeter"}};
        QVERIFY(plankAuthenticatedGreeter(response));
        for (const QString& stage : {QStringLiteral("user"), QStringLiteral("unknown"),
                                     QStringLiteral("closing"), QStringLiteral("Greeter"), QString()}) {
            response["desktop_stage"] = stage;
            QVERIFY(!plankAuthenticatedGreeter(response));
        }
        response["desktop_stage"] = "greeter";
        for (const QString& state : {QStringLiteral("challenge"), QStringLiteral("denied"), QString()}) {
            response["state"] = state;
            QVERIFY(!plankAuthenticatedGreeter(response));
        }
        response["state"] = "authenticated";
        response.remove("session_token");
        QVERIFY(!plankAuthenticatedGreeter(response));
        response["session_token"] = "";
        QVERIFY(!plankAuthenticatedGreeter(response));
        response["session_token"] = 123;
        QVERIFY(!plankAuthenticatedGreeter(response));
        QVERIFY(!plankAuthenticatedGreeter({}));
    }
};

QTEST_GUILESS_MAIN(TestDesktopStage)
#include "test_desktopstage.moc"
