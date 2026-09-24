#include "hosttlsguard.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QSslKey>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <memory>
#include <utility>

namespace {
using Certificate = std::unique_ptr<X509, decltype(&X509_free)>;

Certificate parse(const QSslCertificate& certificate)
{
    const QByteArray bytes = certificate.toDer();
    const auto* cursor = reinterpret_cast<const unsigned char*>(bytes.constData());
    Certificate parsed(d2i_X509(nullptr, &cursor, bytes.size()), X509_free);
    if (cursor != reinterpret_cast<const unsigned char*>(bytes.constData() + bytes.size()))
        parsed.reset();
    return parsed;
}

bool validProfile(const QSslCertificate& certificate, bool leaf)
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    if (certificate.isNull() || certificate.publicKey().algorithm() != QSsl::Rsa ||
            certificate.publicKey().length() < 3072 ||
            certificate.effectiveDate() > now || certificate.expiryDate() <= now) return false;
    if (leaf) {
        const auto sans = certificate.subjectAlternativeNames();
        if (sans.values(QSsl::DnsEntry).isEmpty() || !sans.values(QSsl::IpAddressEntry).isEmpty()) return false;
    }
    return true;
}
}

QByteArray HostTlsGuard::identityKey(const QList<QSslCertificate>& chain)
{
    // Linux has one self-signed machine certificate. macOS presents a worker
    // leaf signed directly by its machine authority. No arbitrary intermediates,
    // downloaded issuers, system CAs or network-provided unsigned key lists.
    if (chain.isEmpty() || chain.size() > 2 || !validProfile(chain.first(), true) ||
            !validProfile(chain.last(), false)) return {};
    auto leaf = parse(chain.first()), root = parse(chain.last());
    if (!leaf || !root) return {};
    EVP_PKEY* rootKey = X509_get0_pubkey(root.get());
    if (!rootKey || X509_NAME_cmp(X509_get_subject_name(root.get()), X509_get_issuer_name(root.get())) ||
            X509_verify(root.get(), rootKey) != 1) return {};
    if (chain.size() == 2) {
        // OpenSSL checks issuer signatures, dates, CA constraints, key usage and
        // server purpose against this candidate anchor. Its identity is pinned
        // separately below; a self-signed root is NOT independently trusted.
        std::unique_ptr<X509_STORE, decltype(&X509_STORE_free)> store(X509_STORE_new(), X509_STORE_free);
        std::unique_ptr<X509_STORE_CTX, decltype(&X509_STORE_CTX_free)> ctx(X509_STORE_CTX_new(), X509_STORE_CTX_free);
        if (!store || !ctx || X509_STORE_add_cert(store.get(), root.get()) != 1 ||
                X509_STORE_CTX_init(ctx.get(), store.get(), leaf.get(), nullptr) != 1 ||
                X509_STORE_CTX_set_purpose(ctx.get(), X509_PURPOSE_SSL_SERVER) != 1 ||
                X509_verify_cert(ctx.get()) != 1 || X509_check_ca(leaf.get()) != 0) return {};
    }
    unsigned char* encoded = nullptr;
    const int size = i2d_PUBKEY(rootKey, &encoded);
    if (size <= 0) return {};
    const QByteArray key = QCryptographicHash::hash(
                QByteArray(reinterpret_cast<const char*>(encoded), size), QCryptographicHash::Sha256);
    OPENSSL_free(encoded);
    return key;
}

QSslConfiguration HostTlsGuard::negotiated(QNetworkReply* reply)
{
    const QVariant saved = reply->property("plankNegotiatedTls");
    return saved.isValid() ? saved.value<QSslConfiguration>() : reply->sslConfiguration();
}

HostTlsGuard::HostTlsGuard(QNetworkAccessManager& manager, const HostTrustStore& store,
                          QString endpoint, Mode mode, QByteArray expectedKey, QByteArray expectedLeaf)
    : m_Store(store), m_Endpoint(std::move(endpoint)), m_Mode(mode),
      m_ExpectedKey(std::move(expectedKey)), m_ExpectedLeaf(std::move(expectedLeaf))
{
    // Keep the manager's thread, but never reuse a socket that could skip the
    // encrypted signal. HTTP redirects are disabled by each request as well.
    manager.clearConnectionCache();
    m_Errors = connect(&manager, &QNetworkAccessManager::sslErrors, this,
        [this](QNetworkReply* reply, const QList<QSslError>& errors) {
        for (const auto& error : errors) {
            switch (error.error()) {
            case QSslError::SelfSignedCertificate:
            case QSslError::SelfSignedCertificateInChain:
            case QSslError::CertificateUntrusted:
            case QSslError::UnableToGetLocalIssuerCertificate:
            case QSslError::UnableToVerifyFirstCertificate:
            case QSslError::HostNameMismatch:
                break;
            default: return;
            }
        }
        if (validate(reply, false)) reply->ignoreSslErrors(errors);
        else reply->abort();
    });
    m_Encrypted = connect(&manager, &QNetworkAccessManager::encrypted, this,
        [this](QNetworkReply* reply) {
        reply->setProperty("plankNegotiatedTls", QVariant::fromValue(reply->sslConfiguration()));
        m_Checked = validate(reply, true);
        if (!m_Checked) reply->abort();
    });
}

HostTlsGuard::~HostTlsGuard()
{
    disconnect(m_Errors);
    disconnect(m_Encrypted);
}

bool HostTlsGuard::validate(QNetworkReply* reply, bool completedHandshake)
{
    const auto ssl = negotiated(reply);
    m_Key = identityKey(ssl.peerCertificateChain());
    if (m_Key.size() != 32 || (completedHandshake && ssl.sessionProtocol() != QSsl::TlsV1_3) ||
            (!m_ExpectedKey.isEmpty() && m_Key != m_ExpectedKey) ||
            (!m_ExpectedLeaf.isEmpty() &&
             ssl.peerCertificate().digest(QCryptographicHash::Sha256) != m_ExpectedLeaf)) {
        m_Result = {HostTrustStore::Status::Error, {}, QStringLiteral(
            "Host TLS identity was rejected. Reconnect to review its identity before signing in.")};
        return false;
    }
    if (m_Mode == Mode::Observe) return true;
    m_Result = m_Store.check(m_Endpoint, m_Key, completedHandshake && m_Mode == Mode::Enroll);
    // Before the handshake finishes we may allow an unknown first-use key;
    // nothing is stored until TLS proves possession and reaches encrypted().
    if (!completedHandshake && m_Mode == Mode::Enroll && m_Result.status == HostTrustStore::Status::Unknown)
        return true;
    return m_Result.status == HostTrustStore::Status::Trusted;
}
