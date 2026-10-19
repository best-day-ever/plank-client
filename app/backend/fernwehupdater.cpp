#include "fernwehupdater.h"
#include "fernwehupdatepolicy.h"
#include "streaming/session.h"
#include <QTimer>

#include "settings/streamingpreferences.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QProcess>
#include <QTemporaryDir>
#include <QThreadPool>

namespace {
#ifdef Q_OS_WIN32
constexpr bool windowsUpdate = true;
#else
constexpr bool windowsUpdate = false;
#endif
const QString feedPath = FernwehUpdatePolicy::feedPath(windowsUpdate);
}

FernwehUpdater::FernwehUpdater(StreamingPreferences* preferences)
    : m_Preferences(preferences)
{
}

bool FernwehUpdater::supported() const
{
#if defined(Q_OS_MACOS) || (defined(Q_OS_WIN32) && defined(_M_X64))
    return true;
#else
    return false;
#endif
}

PlankBrokerClient::Config FernwehUpdater::brokerConfig() const
{
    PlankBrokerClient::Config config;
    config.host = m_Preferences->brokerHost;
    config.port = static_cast<quint16>(m_Preferences->brokerPort);
    config.pins = m_Preferences->brokerPins;
    return config;
}

void FernwehUpdater::check(bool manual)
{
    if (!supported() || m_Busy) return;
    m_Busy = true;
    if (manual) m_Status = tr("Checking for updates…");
    emit changed();
    const auto config = brokerConfig();
    const QPointer<FernwehUpdater> self(this);
    QThreadPool::globalInstance()->start([self, config, manual]() {
        QString version, url, error;
        QByteArray sha;
        qint64 size = 0;
        try {
            const auto response = PlankBrokerClient(config).get(feedPath);
            if (response.status == 200) {
                FernwehUpdatePolicy::Release release;
                if (!FernwehUpdatePolicy::parse(response.body, windowsUpdate, release)) {
                    error = QObject::tr("The update information was invalid.");
                } else if (FernwehUpdatePolicy::newer(release.version, QCoreApplication::applicationVersion())) {
                    version = release.version;
                    url = release.url;
                    sha = release.sha256;
                    size = release.size;
                }
            } else if (response.status != 404) {
                error = QObject::tr("The update server returned an error.");
            }
        } catch (const PlankBrokerError& e) {
            error = e.userMessage();
        }
        QMetaObject::invokeMethod(qApp, [self, version, url, sha, size, error, manual]() {
            if (!self) return;
            self->m_Busy = false;
            if (error.isEmpty()) {
                self->m_Version = version;
                self->m_Url = url;
                self->m_Sha256 = sha;
                self->m_Size = size;
            }
            self->m_Status = !error.isEmpty() ? (manual ? error : QString()) :
                             !version.isEmpty() ? self->tr("Version %1 is ready.").arg(version) :
                             (manual ? self->tr("BDE fernweh is up to date.") : QString());
            emit self->changed();
        });
    });
}

void FernwehUpdater::install()
{
    if (!supported() || m_Busy || m_Version.isEmpty()) return;
    if (waitForStreams()) return;
    QString script, canonicalApp;
#ifndef Q_OS_WIN32
    script = QDir(QCoreApplication::applicationDirPath())
                 .filePath(QStringLiteral("../Resources/fernweh-install-update.sh"));
    if (!QFileInfo::exists(script)) {
        m_Status = tr("The updater is missing from this app.");
        emit changed();
        return;
    }
    canonicalApp = QDir(QCoreApplication::applicationDirPath() + QStringLiteral("/../..")).canonicalPath();
#endif
    m_Busy = true;
    m_Status = tr("Downloading BDE fernweh %1…").arg(m_Version);
    emit changed();
    const auto config = brokerConfig();
    const QString url = m_Url, version = m_Version;
    const QByteArray sha = m_Sha256;
    const qint64 size = m_Size;
    const QPointer<FernwehUpdater> self(this);
    QThreadPool::globalInstance()->start([self, config, url, version, sha, size, script, canonicalApp]() {
        QString file, error;
        QTemporaryDir temp(QDir::tempPath() + QStringLiteral("/bde-fernweh-update-XXXXXX"));
        if (!temp.isValid()) {
            error = QObject::tr("Could not prepare the update download.");
        } else {
            file = temp.filePath(version + (windowsUpdate ? QStringLiteral(".msi") : QStringLiteral(".dmg")));
            try {
                PlankBrokerClient(config).download(url, file, size, sha);
                temp.setAutoRemove(false);
            } catch (const PlankBrokerError& e) {
                error = e.userMessage();
            }
        }
        QMetaObject::invokeMethod(qApp, [self, file, error, script, canonicalApp, version]() {
            if (!self) return;
            if (!error.isEmpty()) {
                self->m_Status = error;
                self->m_Busy = false;
                emit self->changed();
                return;
            }
            // A stream may have started while the background download ran.
            if (Session::get() != nullptr) {
                QFile::remove(file);
                QDir().rmdir(QFileInfo(file).absolutePath());
                self->m_Busy = false;
                self->install();
                return;
            }
#ifdef Q_OS_WIN32
            const QString helper = QDir(QFileInfo(file).absolutePath()).filePath(QStringLiteral("install-update.ps1"));
            const QString powershell = QDir(qEnvironmentVariable("SystemRoot"))
                .filePath(QStringLiteral("System32/WindowsPowerShell/v1.0/powershell.exe"));
            const bool started = QFile::copy(QStringLiteral(":/backend/fernweh-install-update.ps1"), helper) &&
                QProcess::startDetached(powershell, {
                    QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"), QStringLiteral("-STA"),
                    QStringLiteral("-WindowStyle"), QStringLiteral("Hidden"),
                    QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"), QStringLiteral("-File"), helper,
                    QStringLiteral("-Installer"), file, QStringLiteral("-ExpectedHash"), QString::fromLatin1(self->m_Sha256),
                    QStringLiteral("-ExpectedSize"), QString::number(self->m_Size), QStringLiteral("-ExpectedVersion"), version,
                    QStringLiteral("-ParentProcessId"), QString::number(QCoreApplication::applicationPid()),
                    QStringLiteral("-Executable"), QCoreApplication::applicationFilePath()});
            if (!started) QFile::remove(helper);
#else
            const bool started = QProcess::startDetached(QStringLiteral("/bin/bash"),
                {script, file, canonicalApp, QString::number(QCoreApplication::applicationPid()), version});
#endif
            if (!started) {
                QFile::remove(file);
                QDir().rmdir(QFileInfo(file).absolutePath());
                self->m_Status = self->tr("Could not start the installer.");
                self->m_Busy = false;
                emit self->changed();
            } else {
                QCoreApplication::quit();
            }
        });
    });
}

bool FernwehUpdater::waitForStreams()
{
    if (Session::get() == nullptr) return false;
    m_Busy = true;
    m_Status = tr("The update will install after your workstation session ends.");
    emit changed();
    QTimer::singleShot(1000, this, [this]() {
        m_Busy = false;
        install();
    });
    return true;
}
