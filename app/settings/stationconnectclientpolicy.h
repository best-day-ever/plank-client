#pragma once

#include <QString>
#include <QtGlobal>

class StationConnectClientPolicy
{
public:
    explicit StationConnectClientPolicy(
            const QString& configPath = defaultConfigPath());

    static QString defaultConfigPath();

    bool managedBoolean(const QString& key, bool* value) const;
    quint16 networkPort() const;

    static constexpr quint16 BuiltInNetworkPort = 28989;

private:
    QString m_ConfigPath;
};
