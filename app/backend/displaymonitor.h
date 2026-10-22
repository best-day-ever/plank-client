#pragma once

// Watches the client's monitors and reports a settled change: docking,
// undocking, a lid opening, a resolution or arrangement change. macOS reports
// through CGDisplayRegisterReconfigurationCallback, other platforms through
// Qt's screen signals. Changes arrive in bursts (a dock brings several
// displays up one by one), so they are debounced for DebounceMs and only a
// real difference in the probe is reported.

#include "outputtopology.h"

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>

class DisplayMonitor : public QObject
{
    Q_OBJECT

public:
    static constexpr int DebounceMs = 1500;

    explicit DisplayMonitor(QObject* parent = nullptr);
    ~DisplayMonitor() override;

    // The last settled probe, its monitor-set fingerprint and label.
    QVector<NvClientDisplay> displays() const { return m_Displays; }
    QString fingerprint() const { return m_Fingerprint; }
    QString label() const { return m_Label; }

    // Probes again now (GUI thread) and reports a difference like a change.
    void refresh();

signals:
    // setChanged: the monitor set itself changed (a monitor arrived or left);
    // otherwise only sizes or positions did.
    void displaysChanged(QString fingerprint, QString label, bool setChanged);

private slots:
    // CoreGraphics reconfiguration callback, queued onto this object's thread.
    void refreshLater();

private:
    void scheduleCheck();
    void check();
    void watchScreen(QObject* screen);

    QTimer m_Debounce;
    QVector<NvClientDisplay> m_Displays;
    QString m_Fingerprint;
    QString m_Label;
    QString m_Signature;
};
