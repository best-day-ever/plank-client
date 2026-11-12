#include "backend/betadiagnostics.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QSettings>
#include <QJsonDocument>
#include <QFile>
#include <QDir>
#include <QSignalSpy>
#include <QSemaphore>
#include <atomic>
#include <limits>
class BetaDiagnosticsTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { QCoreApplication::setOrganizationName("DiagnosticTests"); QCoreApplication::setApplicationName("BetaDiagnostics"); QCoreApplication::setApplicationVersion("1.2.20-beta"); }
    void init() { QSettings().clear(); }
    void boundedReportsAndNoRawLogFields() {
        QTemporaryDir dir;
        BetaDiagnostics d(dir.path());
        QSignalSpy done(&d,&BetaDiagnostics::workFinished);
        d.setAccount("account",[](const auto&,const auto&) { return 202; });
        d.begin();
        for (int i=0;i<10000;++i) { d.recordEvent("connected"); d.sample(120.0f,150.0f,12,2,3); }
        d.recordEvent("Authorization: Bearer secret",666);
        d.sample(std::numeric_limits<float>::infinity(),-4.0f,999999,999999,999999);
        d.recordEvent("stage_failed",-5); d.finish();
        QTRY_VERIFY_WITH_TIMEOUT(done.size()>=2,3000);
        auto files=QDir(dir.path()).entryList({"*.json"},QDir::Files);
        QCOMPARE(files.size(),1);
        QFile file(QDir(dir.path()).filePath(files[0])); QVERIFY(file.open(QIODevice::ReadOnly));
        auto bytes=file.readAll(); QVERIFY(bytes.size()<BetaDiagnostics::MaxReportBytes); QVERIFY(!bytes.contains("secret"));
        auto report=QJsonDocument::fromJson(bytes).object().value("report").toObject();
        QCOMPARE(report.value("outcome").toString(),QString("failed"));
        QCOMPARE(report.value("events").toArray().size(),32);
        QCOMPARE(report.value("samples").toArray().size(),120);
        auto sample=report.value("samples").toArray().last().toObject();
        QCOMPARE(sample.value("fps_x100").toInt(),0); QCOMPARE(sample.value("rtt_ms").toInt(),60000);
        QVERIFY(!report.contains("identity")); QVERIFY(!report.contains("token"));
    }
    void uploadsAreIdleOnlyAndSuccessRemovesQueue() {
        QTemporaryDir dir; std::atomic_int calls{0};
        BetaDiagnostics d(dir.path()); QSignalSpy done(&d,&BetaDiagnostics::workFinished);
        d.setAccount("account",[&](const auto& report,const auto& cancel) { if (!cancel->load() && report.value("schema").toInt()==1) ++calls; return 202; });
        d.begin(); d.tryUpload(); QCOMPARE(calls.load(),0);
        d.finish(); QTRY_VERIFY(done.size()>=2);
        d.tryUpload(); QTRY_COMPARE(calls.load(),1); QTRY_VERIFY(QDir(dir.path()).entryList({"*.json"},QDir::Files).isEmpty());
    }
    void networkFailureRetainsQueueAndOptOutDeletesIt() {
        QTemporaryDir dir; std::atomic_int calls{0};
        BetaDiagnostics d(dir.path()); QSignalSpy done(&d,&BetaDiagnostics::workFinished);
        d.setAccount("account",[&](const auto&,const auto&) { ++calls; return 503; });
        d.begin(); d.finish(); QTRY_VERIFY(done.size()>=2); d.tryUpload();
        QTRY_COMPARE(calls.load(),1); QTRY_VERIFY(done.size()>=3);
        QCOMPARE(QDir(dir.path()).entryList({"*.json"},QDir::Files).size(),1);
        d.setEnabled(false); QTRY_VERIFY(QDir(dir.path()).entryList({"*.json"},QDir::Files).isEmpty());
        d.begin(); d.finish(); d.tryUpload(); QTest::qWait(30); QCOMPARE(calls.load(),1);
    }
    void reportsNeverCrossAccountsOrBrokerPins() {
        QTemporaryDir dir;
        BetaDiagnostics d(dir.path()); QSignalSpy done(&d,&BetaDiagnostics::workFinished); std::atomic_int calls{0};
        d.setAccount("broker-pins/account1",[](const auto&,const auto&) { return 503; });
        d.begin(); d.finish(); QTRY_VERIFY(done.size()>=2);
        d.setAccount("broker-pins/account2",[&](const auto&,const auto&) { ++calls; return 202; });
        d.tryUpload(); QTRY_VERIFY(done.size()>=3); QCOMPARE(calls.load(),0);
        QCOMPARE(QDir(dir.path()).entryList({"*.json"},QDir::Files).size(),1);
    }
    void completionPublishesOwnerStateBeforeObserversRun() {
        QTemporaryDir dir; BetaDiagnostics d(dir.path());
        std::atomic_int completed{0}, uploads{0};
        std::atomic_bool wrongThread{false};
        d.setAccount("account",[&](const auto&,const auto&) { ++uploads; return 202; });
        connect(&d,&BetaDiagnostics::workFinished,&d,[&] {
            if (QThread::currentThread() != d.thread()) {
                wrongThread = true;
                return; // Never call the collector from an invalid thread.
            }
            // Preparation and final checkpoint have both completed. The
            // observer must be able to start the next operation immediately,
            // without waiting for the separate 15-second idle-upload timer.
            if (++completed == 2) d.tryUpload();
        },Qt::DirectConnection);
        d.begin(); d.finish();
        QTRY_VERIFY(wrongThread.load() || completed.load() >= 3);
        QVERIFY(!wrongThread.load());
        QCOMPARE(completed.load(),3);
        QCOMPARE(uploads.load(),1);
        QVERIFY(QDir(dir.path()).entryList({"*.json"},QDir::Files).isEmpty());
    }
    void checkpointSurvivesInterruptedProcess() {
        QTemporaryDir dir;
        { BetaDiagnostics d(dir.path()); QSignalSpy done(&d,&BetaDiagnostics::workFinished); d.setAccount("account",[](const auto&,const auto&) { return 503; }); d.begin(); QTRY_VERIFY(done.size()>=1); }
        std::atomic_bool recovered{false};
        BetaDiagnostics next(dir.path()); next.setAccount("account",[&](const auto& report,const auto&) { recovered=report.value("outcome")=="interrupted"; return 202; });
        next.tryUpload(); QTRY_VERIFY(recovered.load());
    }
    void localQueueIsBounded() {
        QTemporaryDir dir; BetaDiagnostics d(dir.path()); QSignalSpy done(&d,&BetaDiagnostics::workFinished);
        d.setAccount("account",[](const auto&,const auto&) { return 503; });
        for (int i=0;i<20;++i) { const auto count=done.size(); d.begin(); d.finish(); QTRY_VERIFY(done.size()>=count+2); }
        QCOMPARE(QDir(dir.path()).entryList({"*.json"},QDir::Files).size(),BetaDiagnostics::MaxFiles);
    }
    void startingStreamCancelsAnIdleUpload() {
        QTemporaryDir dir; BetaDiagnostics d(dir.path()); QSignalSpy done(&d,&BetaDiagnostics::workFinished);
        std::atomic_bool started{false},cancelled{false};
        d.setAccount("account",[&](const auto&,const auto& cancel) {
            started=true;
            for (int i=0;i<500 && !cancel->load();++i) QThread::msleep(1);
            cancelled=cancel->load(); return 0;
        });
        d.begin(); d.finish(); QTRY_VERIFY(done.size()>=2);
        d.tryUpload(); QTRY_VERIFY(started.load());
        d.begin(); QTRY_VERIFY(cancelled.load());
        d.finish();
    }
    void obsoleteBrokerAndBadReportsAreNotRetried() {
        for (int status : {400,404,413}) {
            QTemporaryDir dir; BetaDiagnostics d(dir.path()); QSignalSpy done(&d,&BetaDiagnostics::workFinished);
            d.setAccount("account",[status](const auto&,const auto&) { return status; });
            d.begin(); d.finish(); QTRY_VERIFY(done.size()>=2); d.tryUpload();
            QTRY_VERIFY(QDir(dir.path()).entryList({"*.json"},QDir::Files).isEmpty());
        }
    }
    void queuedFileCannotContainCredentialsOrRawMessages() {
        QTemporaryDir dir; BetaDiagnostics d(dir.path()); QSignalSpy done(&d,&BetaDiagnostics::workFinished);
        d.setAccount("private-user@private-broker",[](const auto&,const auto&) { return 503; });
        d.begin(); d.recordEvent("https://private-server/path?token=secret"); d.recordEvent("password=secret"); d.finish();
        QTRY_VERIFY(done.size()>=2);
        const auto files=QDir(dir.path()).entryList({"*.json"},QDir::Files); QCOMPARE(files.size(),1);
        QFile file(QDir(dir.path()).filePath(files[0])); QVERIFY(file.open(QIODevice::ReadOnly)); auto bytes=file.readAll();
        for (const auto forbidden : {"private-user","private-broker","token=","password=","secret","https://"}) QVERIFY(!bytes.contains(forbidden));
    }
    void anotherClientCannotUploadAnActiveCheckpoint() {
        QTemporaryDir dir; BetaDiagnostics active(dir.path()),idle(dir.path());
        QSignalSpy wrote(&active,&BetaDiagnostics::workFinished),sent(&idle,&BetaDiagnostics::workFinished);
        std::atomic_int calls{0};
        active.setAccount("account",[](const auto&,const auto&) { return 503; });
        idle.setAccount("account",[&](const auto&,const auto&) { ++calls; return 202; });
        active.begin(); QTRY_VERIFY(wrote.size()>=1);
        idle.tryUpload(); QTRY_VERIFY(sent.size()>=1); QCOMPARE(calls.load(),0);
        active.finish(); QTRY_VERIFY(wrote.size()>=2);
        idle.tryUpload(); QTRY_COMPARE(calls.load(),1);
    }
    void fastReconnectPreservesBothFinalReports() {
        QTemporaryDir dir; BetaDiagnostics d(dir.path()); QSignalSpy done(&d,&BetaDiagnostics::workFinished);
        d.setAccount("account",[](const auto&,const auto&) { return 503; });
        d.begin(); d.finish(); d.begin(); d.finish();
        QTRY_VERIFY(done.size()>=3);
        auto files=QDir(dir.path()).entryList({"*.json"},QDir::Files); QCOMPARE(files.size(),2);
        for (const auto& name : files) {
            QFile f(QDir(dir.path()).filePath(name)); QVERIFY(f.open(QIODevice::ReadOnly));
            QCOMPARE(QJsonDocument::fromJson(f.readAll()).object().value("report").toObject().value("outcome").toString(),QString("completed"));
        }
    }
    void collectorCost() {
        QTemporaryDir dir; BetaDiagnostics d(dir.path()); d.setAccount("account",[](const auto&,const auto&) { return 503; }); d.begin();
        QBENCHMARK { d.sample(120.0f,150.0f,10,2,3); }
        d.finish();
    }
    void stallReportsRequireObservedStatisticsForThisConnection() {
        QTemporaryDir dir; BetaDiagnostics d(dir.path()); QSignalSpy done(&d,&BetaDiagnostics::workFinished);
        d.setAccount("account",[](const auto&,const auto&) { return 503; });
        d.begin(); d.recordEvent("connected");
        d.sample(0,0,12,2,60000); // Received first frame; no decode statistics yet.
        d.sample(0,0,12,2,14000); // A slow first decoder initialization is not this event.
        d.sample(60,50,12,2,3);   // Actual progress arms steady-state detection.
        d.sample(60,50,12,2,14000);
        d.sample(60,50,12,2,19000); // Same stall, not another event.
        d.recordEvent("connected"); // Reconnect must establish its own progress.
        d.sample(60,50,12,2,24000);
        d.sample(60,50,12,2,3);
        d.sample(60,50,12,60000,14000); // Silence is not a processing stall.
        d.sample(60,50,12,2,14000);
        d.finish(); QTRY_VERIFY(done.size()>=2);
        const auto files=QDir(dir.path()).entryList({"*.json"},QDir::Files); QCOMPARE(files.size(),1);
        QFile file(QDir(dir.path()).filePath(files[0])); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto report=QJsonDocument::fromJson(file.readAll()).object().value("report").toObject();
        int stalls=0;
        for (const auto event : report.value("events").toArray()) {
            if (event.toObject().value("kind").toString()=="suspected_stall") ++stalls;
        }
        QCOMPARE(stalls,2);
    }
};
QTEST_MAIN(BetaDiagnosticsTest)
#include "test_betadiagnostics.moc"
