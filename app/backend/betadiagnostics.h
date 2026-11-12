#pragma once
#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QElapsedTimer>
#include <QTimer>
#include <QThreadPool>
#include <QLockFile>
#include <QList>
#include <atomic>
#include <functional>
#include <memory>

// A separate, allowlisted diagnostic log. Never reads the ordinary text log.
// GUI-thread lifecycle; disk/network work is serialized on one worker.
class BetaDiagnostics : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY changed)
public:
    using Sender = std::function<int(const QJsonObject&, const std::shared_ptr<std::atomic_bool>&)>;
    explicit BetaDiagnostics(QString directory, QObject* parent = nullptr);
    ~BetaDiagnostics() override;
    bool enabled() const { return m_Enabled; }
    void setEnabled(bool enabled);
    void setAccount(const QString& identity, Sender sender);
    void begin();
    void recordEvent(const QString& kind, int code = 0);
    void sample(float fps, float videoMbps, quint32 rttMs, quint32 receivedAgeMs = 0, quint32 statisticsAgeMs = 0);
    void finish();
    // Coalesced per-session checkpoints; both memory and disk queues are bounded.
    void setStreamParameters(const QJsonObject& parameters);
    void checkpoint();
    void tryUpload();
    static QJsonArray displays();
    static QString defaultDirectory();
    static QJsonObject report(const QString& id, const QJsonArray& displays);
    static constexpr int MaxReportBytes = 64 * 1024;
    static constexpr int MaxFiles = 8;
signals:
    void changed();
    void workFinished();
private:
    void dispatchWrite();
    void completeWork();
    QJsonObject snapshot() const;
    QString m_Directory, m_Identity;
    Sender m_Sender;
    bool m_Enabled = true, m_Active = false, m_Busy = false, m_Failed = false, m_StallReported = false;
    bool m_StatisticsObserved = false;
    struct PendingWrite { QJsonObject envelope; std::shared_ptr<QLockFile> lock; };
    QJsonObject m_Report;
    QList<PendingWrite> m_Pending;
    std::shared_ptr<QLockFile> m_ActiveLock;
    QElapsedTimer m_Elapsed;
    QTimer m_Checkpoint, m_Upload;
    QThreadPool m_Worker;
    std::shared_ptr<std::atomic_bool> m_Cancel = std::make_shared<std::atomic_bool>(true);
};
