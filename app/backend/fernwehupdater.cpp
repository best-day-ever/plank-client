#include "fernwehupdater.h"

#include "settings/streamingpreferences.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QVersionNumber>

namespace {
const QString feedPath = QStringLiteral("/v1/client-updates/macos-arm64");
const QRegularExpression versionPattern(QStringLiteral("^[0-9]{1,9}\\.[0-9]{1,9}\\.[0-9]{1,9}$"));
const QRegularExpression shaPattern(QStringLiteral("^[0-9a-f]{64}$"));
}

FernwehUpdater::FernwehUpdater(StreamingPreferences* preferences)
    : m_Preferences(preferences)
{
}

bool FernwehUpdater::supported() const
{
#ifdef Q_OS_MACOS
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
                const auto doc = QJsonDocument::fromJson(response.body);
                const auto object = doc.object();
                version = object.value(QStringLiteral("version")).toString();
                url = object.value(QStringLiteral("url")).toString();
                sha = object.value(QStringLiteral("sha256")).toString().toLatin1();
                size = static_cast<qint64>(object.value(QStringLiteral("size")).toDouble());
                if (!doc.isObject() || !versionPattern.match(version).hasMatch() ||
                        url != feedPath + QLatin1Char('/') + version + QStringLiteral(".dmg") ||
                        !shaPattern.match(QString::fromLatin1(sha)).hasMatch() ||
                        size <= 0 || size > 2LL * 1024 * 1024 * 1024 ||
                        object.value(QStringLiteral("size")).toDouble() != size) {
                    error = QObject::tr("The update information was invalid.");
                    version.clear();
                } else if (QVersionNumber::compare(QVersionNumber::fromString(version),
                                                   QVersionNumber::fromString(QCoreApplication::applicationVersion())) <= 0) {
                    version.clear();
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
    const QString script = QDir(QCoreApplication::applicationDirPath())
                               .filePath(QStringLiteral("../Resources/fernweh-install-update.sh"));
    if (!QFileInfo::exists(script)) {
        m_Status = tr("The updater is missing from this app.");
        emit changed();
        return;
    }
    const QString app = QDir(QCoreApplication::applicationDirPath())
                            .canonicalPath() + QStringLiteral("/../..");
    const QString canonicalApp = QDir(app).canonicalPath();
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
            file = temp.filePath(version + QStringLiteral(".dmg"));
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
            const bool started = QProcess::startDetached(QStringLiteral("/bin/bash"),
                {script, file, canonicalApp, QString::number(QCoreApplication::applicationPid()), version});
            if (!started) {
                self->m_Status = self->tr("Could not start the installer.");
                self->m_Busy = false;
                emit self->changed();
            } else {
                QCoreApplication::quit();
            }
        });
    });
}
