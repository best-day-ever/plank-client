#include "betadiagnostics.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QPointer>
#include <QSaveFile>
#include <QScreen>
#include <QSettings>
#include <QStandardPaths>
#include <QSysInfo>
#include <QThread>
#include <QUuid>
#include <cmath>
#include <utility>
#ifdef Q_OS_WIN32
#include <windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#endif

namespace {
constexpr auto Permissions = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
// Queue files contain no bearer, address, user name or device key. The hash
// binds them to the exact broker pin set and account before a later upload.
bool prepareDirectory(const QString& path) {
    if (!QDir().mkpath(path)) return false;
#ifndef Q_OS_WIN32
    if (!QFile::setPermissions(path, Permissions | QFileDevice::ExeOwner)) return false;
#endif
    return !QFileInfo(path).isSymLink();
}
QFileInfoList prune(const QString& path) {
    auto files = QDir(path).entryInfoList({QStringLiteral("*.json")}, QDir::Files | QDir::NoSymLinks, QDir::Time);
    const auto cutoff = QDateTime::currentDateTimeUtc().addDays(-7);
    for (int i = files.size()-1; i >= 0; --i) {
        if (i >= BetaDiagnostics::MaxFiles || files[i].lastModified() < cutoff || files[i].size() > BetaDiagnostics::MaxReportBytes) {
            QFile::remove(files[i].absoluteFilePath()); files.removeAt(i);
        }
    }
    for (const auto& info : QDir(path).entryInfoList({QStringLiteral("*.lock")},QDir::Files | QDir::NoSymLinks)) {
        if (!QFile::exists(QDir(path).filePath(info.completeBaseName()+".json"))) {
            QLockFile orphan(info.absoluteFilePath()); orphan.setStaleLockTime(0);
            if (orphan.tryLock(0)) orphan.unlock();
        }
    }
    return files;
}
void save(const QString& directory, const QJsonObject& pending) {
    if (!prepareDirectory(directory)) return;
    prune(directory);
    const QString id = pending.value("report").toObject().value("report_id").toString();
    if (id.size() != 32) return;
    const QByteArray bytes = QJsonDocument(pending).toJson(QJsonDocument::Compact);
    if (bytes.size() > BetaDiagnostics::MaxReportBytes) return;
    QSaveFile file(QDir(directory).filePath(id + ".json"));
    file.setDirectWriteFallback(false);
    if (file.open(QIODevice::WriteOnly)) {
        file.setPermissions(Permissions);
        if (file.write(bytes) == bytes.size()) file.commit(); else file.cancelWriting();
    }
    prune(directory);
}
}
QString BetaDiagnostics::defaultDirectory() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath("beta-diagnostics");
}
BetaDiagnostics::BetaDiagnostics(QString directory, QObject* parent) : QObject(parent), m_Directory(std::move(directory)) {
    m_Enabled = QSettings().value("betaDiagnosticsEnabled", true).toBool();
    m_Worker.setMaxThreadCount(1);
    m_Worker.setThreadPriority(QThread::LowPriority);
    m_Worker.setExpiryTimeout(1000);
    m_Checkpoint.setInterval(30000);
    connect(&m_Checkpoint, &QTimer::timeout, this, &BetaDiagnostics::checkpoint);
    m_Upload.setInterval(15 * 60 * 1000);
    connect(&m_Upload, &QTimer::timeout, this, &BetaDiagnostics::tryUpload);
    connect(this, &BetaDiagnostics::workFinished, this, [this] {
        m_Busy = false;
        if (!m_Pending.isEmpty()) dispatchWrite();
    });
    m_Upload.start();
}
BetaDiagnostics::~BetaDiagnostics() {
    m_Cancel->store(true);
    m_Worker.waitForDone(); // Network deadline is 3 seconds; no detached owner.
}
void BetaDiagnostics::setEnabled(bool enabled) {
    if (m_Enabled == enabled) return;
    m_Enabled = enabled;
    QSettings().setValue("betaDiagnosticsEnabled", enabled);
    m_Cancel->store(true);
    m_Checkpoint.stop(); m_Report = {}; m_Pending.clear(); m_ActiveLock.reset();
    if (!enabled) {
        const QString directory = m_Directory;
        m_Worker.start([directory] {
            for (const auto& f : QDir(directory).entryInfoList({QStringLiteral("*.json")}, QDir::Files | QDir::NoSymLinks)) QFile::remove(f.absoluteFilePath());
        });
    } else if (!m_Active) {
        m_Cancel = std::make_shared<std::atomic_bool>(false);
        QTimer::singleShot(15000, this, &BetaDiagnostics::tryUpload);
    }
    emit changed();
}
void BetaDiagnostics::setAccount(const QString& identity, Sender sender) {
    m_Cancel->store(true);
    if (m_Active) { checkpoint(); m_Report = {}; m_Checkpoint.stop(); }
    m_Identity = QString::fromLatin1(QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256).toHex());
    m_Sender = std::move(sender);
    m_Cancel = std::make_shared<std::atomic_bool>(!m_Enabled || m_Active);
    QTimer::singleShot(15000, this, &BetaDiagnostics::tryUpload);
}
QJsonObject BetaDiagnostics::report(const QString& id, const QJsonArray& displays) {
#ifdef Q_OS_WIN32
    const QString platform = "windows";
#elif defined(Q_OS_MACOS)
    const QString platform = "macos";
#else
    const QString platform = "linux";
#endif
    return {{"schema",1},{"stream",QJsonValue::Null},{"report_id",id},{"version",QCoreApplication::applicationVersion()},
            {"platform",platform},{"os_version",QSysInfo::productVersion()},
            {"started_unix",QDateTime::currentSecsSinceEpoch()},{"duration_ms",0},
            {"outcome","interrupted"},{"displays",displays},{"displays_end",QJsonArray()},{"samples",QJsonArray()},{"events",QJsonArray()}};
}
void BetaDiagnostics::begin() {
    m_Cancel->store(true); // Cancels any idle upload before stream preparation.
    m_Active = true; m_Failed = false; m_StallReported = false;
    m_StatisticsObserved = false;
    if (!m_Enabled || m_Identity.isEmpty() || !m_Sender) return;
    m_Report = report(QUuid::createUuid().toString(QUuid::Id128), displays());
    m_ActiveLock = std::make_shared<QLockFile>(QDir(m_Directory).filePath(m_Report.value("report_id").toString()+".lock"));
    m_ActiveLock->setStaleLockTime(0); // PID liveness, never time out a live session.
    m_Elapsed.start(); recordEvent("prepared");
    m_Checkpoint.start(); checkpoint();
}
void BetaDiagnostics::recordEvent(const QString& kind, int code) {
    static const QStringList allowed {"prepared","connected","stage_failed","display_failed","connection_lost","finished","suspected_stall"};
    if (m_Report.isEmpty() || !allowed.contains(kind)) return;
    if (kind == "connected") {
        m_StatisticsObserved = false;
        m_StallReported = false;
    }
    QJsonArray events = m_Report.value("events").toArray();
    if (events.size() >= 32) events.removeAt(0);
    events.append(QJsonObject{{"elapsed_ms",m_Elapsed.elapsed()},{"kind",kind},{"code",code}});
    m_Report.insert("events",events);
    if (kind == "stage_failed" || kind == "display_failed" || kind == "connection_lost") m_Failed = true;
}
void BetaDiagnostics::sample(float fps, float videoMbps, quint32 rttMs, quint32 receivedAgeMs, quint32 statisticsAgeMs) {
    if (m_Report.isEmpty()) return;
    QJsonArray samples = m_Report.value("samples").toArray();
    if (samples.size() >= 120) samples.removeAt(0);
    auto bounded = [](double n, int max) { return std::isfinite(n) ? qBound(0, qRound(qBound(0.0,n,double(max))),max) : 0; };
    samples.append(QJsonObject{{"elapsed_ms",m_Elapsed.elapsed()},{"fps_x100",bounded(fps*100,100000)},
                              {"video_kbps",bounded(videoMbps*1000,10000000)},{"rtt_ms",int(qMin(rttMs,60000u))},
                              {"received_age_ms",int(qMin(receivedAgeMs,60000u))},{"statistics_age_ms",int(qMin(statisticsAgeMs,60000u))}});
    m_Report.insert("samples",samples);
    // Statistics do not exist during negotiation/decoder initialization. A
    // first frame followed by a stale uptime-derived age is not a steady-state
    // stall. Arm only after this connection has actually produced statistics.
    if (receivedAgeMs < 5000 && statisticsAgeMs < 5000) m_StatisticsObserved = true;
    const bool stalled = m_StatisticsObserved && receivedAgeMs < 5000 && statisticsAgeMs > 10000;
    if (stalled && !m_StallReported) recordEvent("suspected_stall");
    m_StallReported = stalled;
}
QJsonObject BetaDiagnostics::snapshot() const {
    auto report = m_Report;
    report.insert("duration_ms",qMin(m_Elapsed.elapsed(),qint64(7*86400*1000)));
    return {{"identity",m_Identity},{"report",report}};
}
void BetaDiagnostics::setStreamParameters(const QJsonObject& parameters) {
    if (!m_Report.isEmpty()) m_Report.insert("stream",parameters);
}
void BetaDiagnostics::checkpoint() {
    if (!m_Enabled || m_Report.isEmpty()) return;
    PendingWrite pending{snapshot(),m_ActiveLock};
    const auto id = pending.envelope.value("report").toObject().value("report_id");
    bool replaced = false;
    for (auto& existing : m_Pending) {
        if (existing.envelope.value("report").toObject().value("report_id") == id) {
            existing = pending; replaced = true; break;
        }
    }
    if (!replaced) {
        if (m_Pending.size() >= MaxFiles) m_Pending.removeFirst();
        m_Pending.append(pending);
    }
    if (!m_Busy) dispatchWrite();
}
void BetaDiagnostics::dispatchWrite() {
    if (m_Pending.isEmpty()) return;
    m_Busy = true;
    const auto pending = m_Pending.takeFirst();
    const auto directory = m_Directory;
    QPointer<BetaDiagnostics> self(this);
    m_Worker.start([self,directory,pending] {
        QSettings preferences; preferences.sync();
        if (preferences.value("betaDiagnosticsEnabled",true).toBool() && prepareDirectory(directory) &&
            pending.lock && (pending.lock->isLocked() || pending.lock->tryLock(0))) {
            save(directory,pending.envelope);
        }
        if (pending.lock && pending.envelope.value("report").toObject().value("outcome") != "interrupted") pending.lock->unlock();
        if (self) emit self->workFinished();
    });
}
void BetaDiagnostics::finish() {
    if (!m_Active) return;
    recordEvent("finished");
    if (!m_Report.isEmpty()) { m_Report.insert("displays_end",displays()); m_Report.insert("outcome",m_Failed ? "failed" : "completed"); checkpoint(); }
    m_Report = {}; m_Checkpoint.stop(); m_Active = false; m_ActiveLock.reset();
    m_Cancel = std::make_shared<std::atomic_bool>(!m_Enabled);
    QTimer::singleShot(15000, this, &BetaDiagnostics::tryUpload);
}
void BetaDiagnostics::tryUpload() {
    if (!m_Enabled || m_Active || m_Busy || !m_Pending.isEmpty() || !m_Sender || m_Cancel->load()) return;
    m_Busy = true;
    const auto directory = m_Directory, identity = m_Identity;
    const auto sender = m_Sender;
    const auto cancelled = m_Cancel;
    QPointer<BetaDiagnostics> self(this);
    m_Worker.start([self,directory,identity,sender,cancelled] {
        QSettings preferences; preferences.sync();
        if (preferences.value("betaDiagnosticsEnabled",true).toBool() && prepareDirectory(directory)) for (const auto& info : prune(directory)) {
            if (cancelled->load()) break;
            QLockFile reportLock(QDir(directory).filePath(info.completeBaseName()+".lock"));
            reportLock.setStaleLockTime(0);
            if (!reportLock.tryLock(0)) continue; // Another client process still owns this session.
            QFile file(info.absoluteFilePath());
            if (!file.open(QIODevice::ReadOnly)) continue;
            const auto document = QJsonDocument::fromJson(file.read(MaxReportBytes+1)).object(); file.close();
            if (document.value("identity").toString() != identity) continue;
            int status = 0;
            try { status = sender(document.value("report").toObject(),cancelled); } catch (...) { }
            if (status == 202 || status == 400 || status == 404 || status == 413) QFile::remove(info.absoluteFilePath());
            // One small upload per idle opportunity; retries every 15 min,
            // including 401/429. No recursion and no authentication mutation.
            break;
        }
        if (self) emit self->workFinished();
    });
}
QJsonArray BetaDiagnostics::displays() {
    QJsonArray result;
    for (const QScreen* screen : QGuiApplication::screens()) {
        if (result.size() >= 16) break;
        QJsonObject display{{"width",qRound(screen->size().width()*screen->devicePixelRatio())},
                            {"height",qRound(screen->size().height()*screen->devicePixelRatio())},
                            {"refresh_millihz",qRound(screen->refreshRate()*1000)},
                            {"hdr_supported",QJsonValue::Null},{"hdr_enabled",QJsonValue::Null},{"bits_per_color",QJsonValue::Null}};
#ifdef Q_OS_WIN32
        // Advanced Color also covers wide-gamut SDR on recent Windows.
        // The active DXGI output color space identifies actual HDR output.
        Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            for (UINT a=0; a<16; ++a) {
                Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
                if (FAILED(factory->EnumAdapters1(a,&adapter))) break;
                for (UINT o=0; o<16; ++o) {
                    Microsoft::WRL::ComPtr<IDXGIOutput> output;
                    if (FAILED(adapter->EnumOutputs(o,&output))) break;
                    Microsoft::WRL::ComPtr<IDXGIOutput6> output6;
                    DXGI_OUTPUT_DESC1 desc{};
                    if (SUCCEEDED(output.As(&output6)) && SUCCEEDED(output6->GetDesc1(&desc)) &&
                        QString::fromWCharArray(desc.DeviceName)==screen->name()) {
                        display.insert("hdr_enabled",desc.ColorSpace==DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
                        display.insert("bits_per_color",int(desc.BitsPerColor));
                    }
                }
            }
        }
#endif
        result.append(display);
    }
    return result;
}
