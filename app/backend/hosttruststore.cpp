#include "hosttruststore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QStandardPaths>

#include <utility>

namespace {
constexpr qint64 MaximumStoreBytes = 2 * 1024 * 1024;
constexpr int MaximumHosts = 4096;
const auto PrivateFile = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
const auto PrivateDirectory = PrivateFile | QFileDevice::ExeOwner;

HostTrustStore::Result failure(const char* message)
{
    return {HostTrustStore::Status::Error, {}, QString::fromLatin1(message)};
}

bool digest(const QString& text)
{
    return text.size() == 64 &&
            QString::fromLatin1(QByteArray::fromHex(text.toLatin1()).toHex()) == text;
}

QString entryKey(const QString& endpoint)
{
    return QString::fromLatin1(QCryptographicHash::hash(endpoint.toUtf8(),
                                                      QCryptographicHash::Sha256).toHex());
}
}

HostTrustStore::HostTrustStore(QString directory) : m_Directory(std::move(directory)) {}

QString HostTrustStore::defaultDirectory()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return base.isEmpty() ? QString() : QDir(base).filePath(QStringLiteral("host-trust"));
}

QString HostTrustStore::endpoint(const QUrl& url)
{
    if (!url.isValid() || url.scheme() != QLatin1String("https") ||
            !url.userInfo().isEmpty() || url.port(-1) <= 0 || url.port() > 65535) return {};
    QString host = url.host();
    QHostAddress ip;
    if (ip.setAddress(host)) host = ip.toString();
    else {
        if (host.endsWith(QLatin1Char('.'))) host.chop(1);
        host = QString::fromLatin1(QUrl::toAce(host)).toLower();
    }
    if (host.isEmpty() || host.size() > 253) return {};
    QUrl canonical;
    canonical.setScheme(QStringLiteral("https"));
    canonical.setHost(host);
    canonical.setPort(url.port());
    return canonical.toString(QUrl::FullyEncoded);
}

QString HostTrustStore::displayFingerprint(const QByteArray& fingerprint)
{
    return fingerprint.size() == 32 ?
                QStringLiteral("SHA256:") + QString::fromLatin1(
                    fingerprint.toBase64(QByteArray::OmitTrailingEquals)) : QString();
}

HostTrustStore::Result HostTrustStore::check(const QString& endpoint, const QByteArray& key,
                                           bool enroll) const
{
    return transact(endpoint, key, enroll, {});
}

HostTrustStore::Result HostTrustStore::replace(const QString& endpoint, const QByteArray& previousKey,
                                             const QByteArray& replacementKey) const
{
    if (previousKey.size() != 32 || previousKey == replacementKey)
        return failure("Invalid Host identity replacement.");
    return transact(endpoint, replacementKey, false, previousKey);
}

HostTrustStore::Result HostTrustStore::transact(const QString& address, const QByteArray& key,
                                              bool enroll, const QByteArray& previousKey) const
{
    if (key.size() != 32 || address.isEmpty() || endpoint(QUrl(address)) != address ||
            !QDir::isAbsolutePath(m_Directory)) return failure("Invalid Host trust state.");

    const QFileInfo directory(m_Directory);
    if (directory.isSymLink() || (directory.exists() && !directory.isDir()))
        return failure("Host trust storage is not a private directory.");
    if (!QDir().mkpath(m_Directory) || !QFile::setPermissions(m_Directory, PrivateDirectory))
        return failure("Cannot access Host trust storage. No credentials were sent.");

    // Serialize across threads AND Client processes. Never let two simultaneous
    // first connections overwrite one another, or a stale dialog replace a newer pin.
    QLockFile lock(QDir(m_Directory).filePath(QStringLiteral("identities.lock")));
    if (!lock.tryLock(2000)) return failure("Host trust storage is busy. Try connecting again.");
    const QString path = QDir(m_Directory).filePath(QStringLiteral("identities.json"));
    const QFileInfo info(path);
    if (info.isSymLink() || (info.exists() && !info.isFile()))
        return failure("Invalid Host trust storage. Stored identities were not reset.");
    QJsonObject entries;
    if (info.exists()) {
        QFile input(path);
        if (!input.open(QIODevice::ReadOnly) || input.size() > MaximumStoreBytes)
            return failure("Cannot read Host trust storage. Stored identities were not reset.");
        QJsonParseError parse;
        const auto document = QJsonDocument::fromJson(input.read(MaximumStoreBytes + 1), &parse);
        const auto root = document.object();
        if (parse.error != QJsonParseError::NoError || !document.isObject() || root.size() != 2 ||
                root.value(QStringLiteral("version")) != QJsonValue(1) ||
                !root.value(QStringLiteral("hosts")).isObject())
            return failure("Damaged Host trust storage. Stored identities were not reset.");
        entries = root.value(QStringLiteral("hosts")).toObject();
        if (entries.size() > MaximumHosts)
            return failure("Host trust storage exceeded its limit.");
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            const auto record = it.value().toObject();
            const QString storedAddress = record.value(QStringLiteral("endpoint")).toString();
            if (!it.value().isObject() || record.size() != 2 || storedAddress.isEmpty() ||
                    endpoint(QUrl(storedAddress)) != storedAddress || entryKey(storedAddress) != it.key() ||
                    !digest(record.value(QStringLiteral("key_sha256")).toString()))
                return failure("Damaged Host trust storage. Stored identities were not reset.");
        }
    }
    const QString id = entryKey(address);
    const QByteArray saved = QByteArray::fromHex(entries.value(id).toObject()
                                                .value(QStringLiteral("key_sha256")).toString().toLatin1());
    if (saved == key) return {Status::Trusted, saved, {}};
    if (!previousKey.isEmpty()) {
        if (saved != previousKey)
            return failure("Host identity changed again. Connect again to review the current identity.");
    } else if (!saved.isEmpty()) return {Status::Changed, saved, {}};
    else if (!enroll) return {Status::Unknown, {}, {}};
    if (saved.isEmpty() && entries.size() >= MaximumHosts)
        return failure("Host trust storage exceeded its limit.");

    entries[id] = QJsonObject{{QStringLiteral("endpoint"), address},
                             {QStringLiteral("key_sha256"), QString::fromLatin1(key.toHex())}};
    const QByteArray bytes = QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
        {QStringLiteral("hosts"), entries}}).toJson(QJsonDocument::Compact);
    if (bytes.size() > MaximumStoreBytes) return failure("Host trust storage exceeded its limit.");
    QSaveFile output(path);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly) || !output.setPermissions(PrivateFile) ||
            output.write(bytes) != bytes.size() || !output.commit())
        return failure("Cannot save Host identity. No credentials were sent.");
    return {Status::Trusted, key, {}};
}
