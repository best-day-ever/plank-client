#pragma once

#include <QObject>
#include <QString>

#include "plankbrokerclient.h"

class StreamingPreferences;

// Checks the trusted broker for a newer Mac release and installs it on click.
class FernwehUpdater : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool supported READ supported CONSTANT)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(QString version READ version NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)

public:
    explicit FernwehUpdater(StreamingPreferences* preferences);

    bool supported() const;
    bool busy() const { return m_Busy; }
    bool available() const { return !m_Version.isEmpty(); }
    QString version() const { return m_Version; }
    QString status() const { return m_Status; }

    Q_INVOKABLE void check(bool manual = false);
    Q_INVOKABLE void install();

signals:
    void changed();

private:
    PlankBrokerClient::Config brokerConfig() const;
    StreamingPreferences* m_Preferences;
    bool m_Busy = false;
    QString m_Version;
    QString m_Url;
    QByteArray m_Sha256;
    qint64 m_Size = 0;
    QString m_Status;
};
