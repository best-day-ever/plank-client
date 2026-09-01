#include "plankclientpolicy.h"

#include <QSettings>
#include <QtDebug>

PlankClientPolicy::PlankClientPolicy(const QString& configPath)
    : m_ConfigPath(configPath)
{
}

QString PlankClientPolicy::defaultConfigPath()
{
    return QStringLiteral("/etc/plank/client.conf");
}

bool PlankClientPolicy::managedBoolean(const QString& key, bool* value) const
{
    Q_ASSERT(value != nullptr);

    QSettings policy(m_ConfigPath, QSettings::IniFormat);
    if (!policy.contains(key)) {
        return false;
    }

    const QString configuredValue = policy.value(key).toString().trimmed().toLower();
    if (configuredValue == QStringLiteral("true")) {
        *value = true;
    }
    else if (configuredValue == QStringLiteral("false")) {
        *value = false;
    }
    else {
        qWarning() << "Invalid managed boolean" << configuredValue
                   << "for" << key << "in" << m_ConfigPath
                   << "- forcing false";
        *value = false;
    }

    return true;
}

quint16 PlankClientPolicy::networkPort() const
{
    QSettings policy(m_ConfigPath, QSettings::IniFormat);
    bool valid = false;
    const int configuredPort = policy.value(
                QStringLiteral("network/port"), BuiltInNetworkPort).toInt(&valid);
    if (!valid || configuredPort < 1024 || configuredPort > 65535) {
        qWarning() << "Invalid PLANK network port" << configuredPort
                   << "in" << m_ConfigPath << "- using"
                   << BuiltInNetworkPort;
        return BuiltInNetworkPort;
    }

    return static_cast<quint16>(configuredPort);
}
