#pragma once

#include <QString>

class StationConnectClientPolicy
{
public:
    explicit StationConnectClientPolicy(
            const QString& configPath = defaultConfigPath());

    static QString defaultConfigPath();

    bool managedBoolean(const QString& key, bool* value) const;

private:
    QString m_ConfigPath;
};
