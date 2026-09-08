#include <QtTest>
#include "desktopstage.h"
#include "hostrecovery.h"

class TestDesktopStage : public QObject
{
    Q_OBJECT
private slots:
    void boundsSilenceTrigger()
    {
        using namespace PlankHostRecovery;
        QVERIFY(!videoSilent(10000, 0)); // No first frame yet.
        QVERIFY(!videoSilent(999, 1000));
        QVERIFY(!videoSilent(1999, 1000));
        QVERIFY(videoSilent(2000, 1000));
        QVERIFY(!videoSilent(2001, 2000)); // Incoming video cancels recovery.
    }

    void requiresPinnedCertificateAndReplacement()
    {
        using namespace PlankHostRecovery;
        const QString first = "11111111-1111-4111-8111-111111111111";
        const QString second = "22222222-2222-4222-8222-222222222222";
        const QByteArray certificate(32, 'a');
        QVERIFY(replacementConfirmed(first, second, certificate, certificate));
        QVERIFY(!replacementConfirmed(first, first, certificate, certificate));
        QVERIFY(!replacementConfirmed(first, second, certificate, QByteArray(32, 'b')));
        QVERIFY(!replacementConfirmed(first, second, {}, {}));
        for (const QString& invalid : {QString(), QStringLiteral("not-an-instance"),
                                       QStringLiteral("00000000-0000-0000-0000-000000000000"),
                                       "{" + second + "}"}) {
            QVERIFY(!replacementConfirmed(first, invalid, certificate, certificate));
            QVERIFY(!replacementConfirmed(invalid, second, certificate, certificate));
        }
    }

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
