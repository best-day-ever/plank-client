#pragma once

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QLockFile>
#include <QProcess>
#include <QStandardPaths>
#include <QUuid>
#include <memory>

// The streaming libraries own process-global callbacks. Keep one stream per
// process and coordinate application replacement across all launcher windows.
class ClientInstances
{
public:
    explicit ClientInstances(const QString& directory)
        : m_Directory(directory), m_UpdateLock(directory + "/update.lock"),
          m_InstancePath(directory + "/instance-" +
                         QUuid::createUuid().toString(QUuid::WithoutBraces) + ".lock")
    {
        m_UpdateLock.setStaleLockTime(0);
    }

    static ClientInstances& get()
    {
        static ClientInstances registry(QStandardPaths::writableLocation(
            QStandardPaths::AppLocalDataLocation) + "/instances");
        return registry;
    }

    bool registerInstance()
    {
        if (m_Instance) return true;
        if (!QDir().mkpath(m_Directory)) return false;
#ifndef Q_OS_WIN
        if (!QFile::setPermissions(m_Directory, QFile::ReadOwner |
                                  QFile::WriteOwner | QFile::ExeOwner)) return false;
#endif
        // Serialize registration with the updater's final process census.
        if (!m_UpdateLock.tryLock()) return false;
        auto instance = std::make_unique<QLockFile>(m_InstancePath);
        instance->setStaleLockTime(0);
        const bool registered = instance->tryLock();
        if (registered) m_Instance = std::move(instance);
        m_UpdateLock.unlock();
        return registered;
    }

    bool hasOtherInstances() const
    {
        if (!m_Instance) return true; // fail closed when registration failed
        const QDir directory(m_Directory);
        for (const QString& file : directory.entryList({"instance-*.lock"}, QDir::Files)) {
            const QString path = directory.filePath(file);
            if (path == m_InstancePath) continue;
            QLockFile probe(path);
            probe.setStaleLockTime(0);
            // QLockFile removes locks left by dead processes. A live process
            // or an unreadable lock blocks replacement; never guess a PID.
            if (!probe.tryLock()) return true;
        }
        return false;
    }

    bool beginUpdate()
    {
        if (m_Updating) return true;
        if (!m_Instance || !m_UpdateLock.tryLock()) return false;
        if (hasOtherInstances()) {
            m_UpdateLock.unlock();
            return false;
        }
        m_Updating = true;
        return true;
    }

    void endUpdate()
    {
        if (!m_Updating) return;
        m_Updating = false;
        m_UpdateLock.unlock();
    }

    bool openNewConnection()
    {
        if (!m_Instance || m_Updating || !m_UpdateLock.tryLock()) return false;
        m_UpdateLock.unlock();
        // No credentials, lease or session keys in argv, environment or disk.
        // The new launcher resumes the existing protected broker sign-in.
        return QProcess::startDetached(QCoreApplication::applicationFilePath(), {});
    }

private:
    QString m_Directory;
    QLockFile m_UpdateLock;
    QString m_InstancePath;
    std::unique_ptr<QLockFile> m_Instance;
    bool m_Updating = false;
};
