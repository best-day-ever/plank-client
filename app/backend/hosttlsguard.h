#pragma once

#include "hosttruststore.h"
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSslCertificate>
#include <QSslConfiguration>

// Per-request pre-send gate. Never approve credentials from a reply-time check.
// Callers must not overlap requests on the supplied manager while this lives.
class HostTlsGuard : public QObject
{
public:
    enum class Mode { Observe, Enroll, RequireKnown };
    HostTlsGuard(QNetworkAccessManager& manager, const HostTrustStore& store,
                 QString endpoint, Mode mode, QByteArray expectedKey = {},
                 QByteArray expectedLeaf = {});
    ~HostTlsGuard() override;
    static QByteArray identityKey(const QList<QSslCertificate>& chain);
    static QSslConfiguration negotiated(QNetworkReply* reply);
    bool checked() const { return m_Checked; }
    const HostTrustStore::Result& result() const { return m_Result; }
    const QByteArray& key() const { return m_Key; }

private:
    bool validate(QNetworkReply* reply, bool completedHandshake);
    const HostTrustStore& m_Store;
    QString m_Endpoint;
    Mode m_Mode;
    QByteArray m_ExpectedKey, m_ExpectedLeaf, m_Key;
    HostTrustStore::Result m_Result;
    bool m_Checked = false;
    QMetaObject::Connection m_Encrypted, m_Errors;
};
