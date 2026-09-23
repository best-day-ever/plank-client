#include <QtTest>
#include "desktopstage.h"
#include "hostrecovery.h"
#include "../../app/streaming/plankreconnectpolicy.h"

class TestDesktopStage : public QObject
{
    Q_OBJECT
private slots:
    void reconnectWaitDeadline()
    {
        PlankReconnectPolicy policy;
        QVERIFY(!policy.allowsRequest(0));
        policy.allowUntil(30000);
        QVERIFY(policy.allowsRequest(29999));
        QVERIFY(!policy.allowsRequest(30000));
        QVERIFY(!policy.allowsRequest(60000)); // Unanswered prompt remains paused.
        policy.allowUntil(90000); // Explicit Keep Waiting, not a timer retry.
        QVERIFY(policy.allowsRequest(60000));
        QVERIFY(!policy.allowsRequest(90000));
    }

    void reconnectRejectionPolicy()
    {
        for (int status : {400, 403, 404, 423}) {
            QVERIFY(PlankReconnectPolicy::terminalStatus(status, true));
            QVERIFY(PlankReconnectPolicy::terminalStatus(status, false));
        }
        QVERIFY(PlankReconnectPolicy::terminalStatus(401, true));
        QVERIFY(!PlankReconnectPolicy::terminalStatus(401, true, true)); // Confirmed desktop handover or sign-out.
        QVERIFY(PlankReconnectPolicy::terminalStatus(403, true, true));
        QVERIFY(PlankReconnectPolicy::terminalStatus(423, true, true));
        QVERIFY(!PlankReconnectPolicy::terminalStatus(401, false)); // Expired worker token.
        for (int status : {409, 425, 429, 500, 502, 503, 504}) {
            QVERIFY(!PlankReconnectPolicy::terminalStatus(status, true));
            QVERIFY(!PlankReconnectPolicy::terminalStatus(status, false));
        }
    }

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

    void parsesDesktopSignOut()
    {
        using State = PlankDesktopSignOut::State;
        const QString alice = QStringLiteral("alice");
        PlankDesktopSignOut reply = PlankDesktopSignOut::fromResponse(409, alice, "available");
        QCOMPARE(reply.state, State::Available);
        QCOMPARE(reply.owner, alice);
        QCOMPARE(PlankDesktopSignOut::fromResponse(409, alice, "connected").state, State::Connected);
        QCOMPARE(PlankDesktopSignOut::fromResponse(503, alice, "started").state, State::Started);
        QCOMPARE(PlankDesktopSignOut::fromResponse(409, "alice@ipa.example", "available").owner,
                 QStringLiteral("alice@ipa.example"));

        // Status and offer must agree; other launch refusals are not ours.
        QCOMPARE(PlankDesktopSignOut::fromResponse(503, alice, "available").state, State::None);
        QCOMPARE(PlankDesktopSignOut::fromResponse(409, alice, "started").state, State::None);
        QCOMPARE(PlankDesktopSignOut::fromResponse(200, alice, "available").state, State::None);
        QCOMPARE(PlankDesktopSignOut::fromResponse(409, alice, "Available").state, State::None);
        QCOMPARE(PlankDesktopSignOut::fromResponse(409, alice, QString()).state, State::None);
        for (const QString& owner : {QString(), QStringLiteral("alice bob"), QStringLiteral(" alice"),
                                     QStringLiteral("alice\n"), QStringLiteral("a\tb"),
                                     QString(257, QChar('a'))}) {
            reply = PlankDesktopSignOut::fromResponse(409, owner, "available");
            QCOMPARE(reply.state, State::None);
            QVERIFY(reply.owner.isEmpty());
        }
    }

    void decidesDesktopSignOutStep()
    {
        using Step = PlankDesktopSignOut::Step;
        const auto reply = [](int status, const char* owner, const char* offer) {
            return PlankDesktopSignOut::fromResponse(status, owner, offer);
        };
        const auto step = PlankDesktopSignOut::nextStep;
        const QString none;
        const QString alice = QStringLiteral("alice");

        QCOMPARE(step({}, false, true, none), Step::NotApplicable);
        // Available: offer once, and only with credentials for the reconnect.
        QCOMPARE(step(reply(409, "alice", "available"), false, true, none), Step::Confirm);
        QCOMPARE(step(reply(409, "alice", "available"), false, false, none), Step::InUse);
        QCOMPARE(step(reply(409, "alice", "available"), true, true, none), Step::InUse);
        QCOMPARE(step(reply(409, "alice", "available"), false, true, alice), Step::Unexpected);
        // The owner changed between the offer and the confirmed retry.
        QCOMPARE(step(reply(409, "bob", "available"), false, true, alice), Step::Confirm);
        // Connected never offers sign-out, before or after a retry.
        QCOMPARE(step(reply(409, "alice", "connected"), false, true, none), Step::InUse);
        QCOMPARE(step(reply(409, "bob", "connected"), false, true, alice), Step::InUse);
        QCOMPARE(step(reply(409, "alice", "connected"), true, true, none), Step::InUse);
        // Started only answers the sign-out this Client asked for.
        QCOMPARE(step(reply(503, "alice", "started"), false, true, alice), Step::AwaitGreeter);
        QCOMPARE(step(reply(503, "alice", "started"), false, true, none), Step::Unexpected);
        QCOMPARE(step(reply(503, "bob", "started"), false, true, alice), Step::Unexpected);

        // While waiting for the sign-in screen.
        QVERIFY(PlankDesktopSignOut::stillSigningOut(reply(409, "alice", "available"), alice));
        QVERIFY(!PlankDesktopSignOut::stillSigningOut(reply(409, "bob", "available"), alice));
        QVERIFY(!PlankDesktopSignOut::stillSigningOut(reply(409, "alice", "connected"), alice));
        QVERIFY(!PlankDesktopSignOut::stillSigningOut(reply(409, "alice", "available"), none));
        QVERIFY(!PlankDesktopSignOut::stillSigningOut({}, alice));
    }
};

QTEST_GUILESS_MAIN(TestDesktopStage)
#include "test_desktopstage.moc"
