#pragma once

// Remembers the remote-access (broker) session across Client restarts, so
// people are not asked for password + code (or Touch ID) every time they
// open the app. Only the broker's opaque session token and the confirmed
// user name are kept -- never a password, OTP or passkey material -- and only
// in the macOS login Keychain (this device only, never synced). Other
// platforms keep the session in memory as before. The broker still bounds the
// session (absolute and idle lifetime); an expired token is simply dropped.

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

namespace BrokerSessionStore {

struct Saved
{
    QString username;
    QString token;
};

// Pure encoding, kept separate from the Keychain for tests.
inline QByteArray encode(const QString& username, const QString& token)
{
    QJsonObject object;
    object.insert(QStringLiteral("v"), 1);
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
    if (object.value(QStringLiteral("v")).toInt() != 1) return false;
    const QString username = object.value(QStringLiteral("user")).toString();
    const QString token = object.value(QStringLiteral("token")).toString();
    if (username.isEmpty() || username.size() > 255 || token.isEmpty() || token.size() > 512) return false;
    for (const QChar c : token) {
        if (c.unicode() < 0x21 || c.unicode() > 0x7e) return false;
    }
    saved.username = username;
    saved.token = token;
    return true;
}

// One saved session per broker address ("host:port"). All return false where
// persistence is unavailable (non-Apple platforms, Keychain locked/denied).
bool isAvailable();
bool save(const QString& brokerAddress, const QString& username, const QString& token);
bool load(const QString& brokerAddress, Saved& saved);
void clear(const QString& brokerAddress);

}
