#pragma once

#include <QByteArray>
#include <QString>
#include <QUrl>

// Per-local-user trust, deliberately independent of bookmark/settings storage.
// Call only with an identity whose certificate/signature has been validated.
// Discovery must not enroll identities. Only an explicit connection may TOFU.
class HostTrustStore
{
public:
    enum class Status { Trusted, Unknown, Changed, Error };
    struct Result {
        Status status = Status::Error;
        QByteArray previousKey;
        QString error;
    };

    explicit HostTrustStore(QString directory = defaultDirectory());
    static QString defaultDirectory();
    static QString endpoint(const QUrl& url);
    static QString displayFingerprint(const QByteArray& fingerprint);

    Result check(const QString& endpoint, const QByteArray& key, bool enroll = false) const;
    // Never forget-and-relearn: consent applies to exactly the two keys shown.
    Result replace(const QString& endpoint, const QByteArray& previousKey,
                   const QByteArray& replacementKey) const;

private:
    Result transact(const QString& endpoint, const QByteArray& key, bool enroll,
                    const QByteArray& previousKey) const;
    QString m_Directory;
};
