#pragma once

// Remembers the remote-access (broker) session across Client restarts, so
// people are not asked for password + code (or Touch ID) every time they
// open the app. Only the broker's opaque session token and the confirmed
// user name and device-binding status are kept -- never a password, OTP or passkey material -- and only
// in the macOS login Keychain (this device only, never synced) or Windows
// current-user DPAPI storage. Other platforms keep the session in memory.
// The broker controls lifetime and revocation. A username preference is kept
// separately so expiry or explicit sign-out does not erase a successful name.

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QCryptographicHash>
#include <QSettings>

namespace BrokerSessionStore {

struct Saved
{
    QString username;
    QString token;
    bool deviceBound = false;
    bool legacy = true;
};

// Pure encoding, kept separate from the Keychain for tests.
inline QByteArray encode(const QString& username, const QString& token, bool deviceBound = false)
{
    QJsonObject object;
    object.insert(QStringLiteral("v"), 2);
    object.insert(QStringLiteral("device_bound"), deviceBound);
    object.insert(QStringLiteral("user"), username);
    object.insert(QStringLiteral("token"), token);
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

inline bool decode(const QByteArray& data, Saved& saved)
{
    saved = Saved();
    if (data.isEmpty() || data.size() > 4096) return false;
    QJsonParseError error {};
    const QJsonDocument document = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
    const QJsonObject object = document.object();
    const auto version = object.value(QStringLiteral("v")).toInt();
    if (version != 1 && version != 2) return false;
    if (version == 2 && !object.value(QStringLiteral("device_bound")).isBool()) return false;
    const QString username = object.value(QStringLiteral("user")).toString();
    const QString token = object.value(QStringLiteral("token")).toString();
    if (username.isEmpty() || username.size() > 255 || token.isEmpty() || token.size() > 512) return false;
    for (const QChar c : token) {
        if (c.unicode() < 0x21 || c.unicode() > 0x7e) return false;
    }
    saved.username = username;
    saved.token = token;
    saved.legacy = version == 1;
    saved.deviceBound = version == 2 && object.value(QStringLiteral("device_bound")).toBool();
    return true;
}

// One saved session per broker address ("host:port"). Returns false where
// persistence is unavailable or protected storage refuses the operation.
bool isAvailable();
bool save(const QString& brokerAddress, const QString& username, const QString& token, bool deviceBound = false);
bool load(const QString& brokerAddress, Saved& saved);
void clear(const QString& brokerAddress);

// Non-secret, independent of the expiring/revocable credential. Per broker,
// recorded only after a successful sign-in; never a password or OTP.
inline QString usernameKey(const QString& authority)
{
    return QStringLiteral("remote-usernames/") + QString::fromLatin1(
                QCryptographicHash::hash(authority.trimmed().toLower().toUtf8(), QCryptographicHash::Sha256).toHex());
}
inline QString rememberedUsername(const QString& authority)
{
    return QSettings().value(usernameKey(authority)).toString();
}
inline void rememberUsername(const QString& authority, const QString& user)
{
    if (!authority.isEmpty() && !user.isEmpty() && user.size() <= 255)
        QSettings().setValue(usernameKey(authority), user);
}

inline bool needsDeviceSignIn(const Saved& saved)
{
#ifdef Q_OS_WIN
    return saved.legacy || !saved.deviceBound;
#else
    Q_UNUSED(saved)
    return false;
#endif
}

}
