#include "brokerdevicekey.h"

#include <QCryptographicHash>
#include <QDebug>
#include <cstring>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <ncrypt.h>
#include <bcrypt.h>
#endif

namespace BrokerDeviceKey {

QByteArray p256Spki(const QByteArray& point)
{
    if (point.size() != 65 || point.front() != '\x04' || point.mid(1) == QByteArray(64, '\0')) return {};
    return QByteArray::fromHex("3059301306072a8648ce3d020106082a8648ce3d030107034200") + point;
}

QByteArray ecdsaDer(const QByteArray& raw)
{
    if (raw.size() != 64) return {};
    const auto integer = [](QByteArray value) {
        while (value.size() > 1 && value.front() == '\0') value.remove(0, 1);
        if (value == QByteArray(1, '\0')) return QByteArray();
        if (static_cast<unsigned char>(value.front()) & 0x80) value.prepend('\0');
        return QByteArray(1, '\x02') + QByteArray(1, char(value.size())) + value;
    };
    const auto r = integer(raw.left(32)), s = integer(raw.mid(32));
    if (r.isEmpty() || s.isEmpty()) return {};
    return QByteArray(1, '\x30') + QByteArray(1, char(r.size() + s.size())) + r + s;
}

QString WindowsKey::keyName(const QString& authority)
{
    return QStringLiteral("la.instinctual.PLANK.Client.device-proof.v1.") +
            QString::fromLatin1(QCryptographicHash::hash(authority.toUtf8(), QCryptographicHash::Sha256).toHex());
}

#ifdef Q_OS_WIN
namespace {
struct Handle {
    NCRYPT_HANDLE value = 0;
    ~Handle() { if (value) NCryptFreeObject(value); }
};

bool missing(SECURITY_STATUS status) { return status == NTE_BAD_KEYSET || status == NTE_NOT_FOUND; }

SECURITY_STATUS openKey(const QString& authority, Handle& provider, Handle& key, bool create)
{
    auto status = NCryptOpenStorageProvider(&provider.value, MS_KEY_STORAGE_PROVIDER, 0);
    if (status != ERROR_SUCCESS) return status;
    const QString name = WindowsKey::keyName(authority);
    const auto* nativeName = reinterpret_cast<const wchar_t*>(name.utf16());
    status = NCryptOpenKey(provider.value, &key.value, nativeName, 0, NCRYPT_SILENT_FLAG);
    if (status == ERROR_SUCCESS || !create || !missing(status)) return status;
    // Current user only; no overwrite. A concurrent first login must reopen the
    // winning key, rather than replace a key another session already uses.
    status = NCryptCreatePersistedKey(provider.value, &key.value, NCRYPT_ECDSA_P256_ALGORITHM,
                                     nativeName, 0, 0);
    if (status == NTE_EXISTS) return NCryptOpenKey(provider.value, &key.value, nativeName, 0, NCRYPT_SILENT_FLAG);
    if (status != ERROR_SUCCESS) return status;
    DWORD exportPolicy = 0, usage = NCRYPT_ALLOW_SIGNING_FLAG;
    status = NCryptSetProperty(key.value, NCRYPT_EXPORT_POLICY_PROPERTY,
                              reinterpret_cast<PBYTE>(&exportPolicy), sizeof(exportPolicy), 0);
    if (status == ERROR_SUCCESS)
        status = NCryptSetProperty(key.value, NCRYPT_KEY_USAGE_PROPERTY,
                                  reinterpret_cast<PBYTE>(&usage), sizeof(usage), 0);
    if (status == ERROR_SUCCESS) status = NCryptFinalizeKey(key.value, NCRYPT_SILENT_FLAG);
    return status;
}

QByteArray publicSpki(NCRYPT_KEY_HANDLE key)
{
    DWORD size = 0, policy = 0, usage = 0, actual = 0;
    if (NCryptGetProperty(key, NCRYPT_EXPORT_POLICY_PROPERTY, reinterpret_cast<PBYTE>(&policy),
                         sizeof(policy), &actual, 0) != ERROR_SUCCESS || actual != sizeof(policy) || policy != 0 ||
        NCryptGetProperty(key, NCRYPT_KEY_USAGE_PROPERTY, reinterpret_cast<PBYTE>(&usage),
                         sizeof(usage), &actual, 0) != ERROR_SUCCESS || actual != sizeof(usage) ||
        usage != NCRYPT_ALLOW_SIGNING_FLAG ||
        NCryptExportKey(key, 0, BCRYPT_ECCPUBLIC_BLOB, nullptr, nullptr, 0, &size, 0) != ERROR_SUCCESS ||
        size != sizeof(BCRYPT_ECCKEY_BLOB) + 64) return {};
    QByteArray blob(int(size), '\0');
    if (NCryptExportKey(key, 0, BCRYPT_ECCPUBLIC_BLOB, nullptr,
                       reinterpret_cast<PBYTE>(blob.data()), size, &actual, 0) != ERROR_SUCCESS || actual != size) return {};
    BCRYPT_ECCKEY_BLOB header {};
    std::memcpy(&header, blob.constData(), sizeof(header));
    if (header.dwMagic != BCRYPT_ECDSA_PUBLIC_P256_MAGIC || header.cbKey != 32) return {};
    return p256Spki(QByteArray(1, '\x04') + blob.mid(sizeof(header)));
}
}
#endif

QString WindowsKey::publicKey() const
{
#ifdef Q_OS_WIN
    Handle provider, key;
    if (openKey(m_Authority, provider, key, true) != ERROR_SUCCESS) return {};
    return QString::fromLatin1(publicSpki(key.value).toBase64());
#else
    return {};
#endif
}

PlankBrokerClient::DeviceSignature WindowsKey::sign(const QByteArray& message) const
{
    using Signature = PlankBrokerClient::DeviceSignature;
#ifdef Q_OS_WIN
    if (message.isEmpty()) return {Signature::Failed, {}};
    Handle provider, key;
    const auto status = openKey(m_Authority, provider, key, false);
    if (missing(status)) return {Signature::NoKey, {}};
    if (status != ERROR_SUCCESS || publicSpki(key.value).isEmpty()) return {Signature::Failed, {}};
    QByteArray hash = QCryptographicHash::hash(message, QCryptographicHash::Sha256);
    QByteArray raw(64, '\0');
    DWORD actual = 0;
    if (NCryptSignHash(key.value, nullptr, reinterpret_cast<PBYTE>(hash.data()), DWORD(hash.size()),
                      reinterpret_cast<PBYTE>(raw.data()), DWORD(raw.size()), &actual,
                      NCRYPT_SILENT_FLAG) != ERROR_SUCCESS || actual != 64 ||
        NCryptVerifySignature(key.value, nullptr, reinterpret_cast<PBYTE>(hash.data()), DWORD(hash.size()),
                             reinterpret_cast<PBYTE>(raw.data()), DWORD(raw.size()), 0) != ERROR_SUCCESS)
        return {Signature::Failed, {}};
    const auto der = ecdsaDer(raw);
    return der.isEmpty() ? Signature{Signature::Failed, {}} :
                          Signature{Signature::Signed, QString::fromLatin1(der.toBase64())};
#else
    Q_UNUSED(message)
    return {Signature::NoKey, {}};
#endif
}

PlankBrokerClient::Config clientConfig(const QString& host, quint16 port, const QStringList& pins,
                                     std::function<bool()> proofRequired, const QString& macHelper)
{
    PlankBrokerClient::Config config;
    config.host = host;
    config.port = port;
    config.pins = pins;
    config.deviceProofRequired = std::move(proofRequired);
    const auto deviceHost = host.trimmed().toLower();
#ifdef Q_OS_WIN
    if (!PlankBroker::isEndpointName(deviceHost)) return config;
    Q_UNUSED(macHelper)
    const WindowsKey key(QStringLiteral("%1:%2").arg(deviceHost).arg(port));
    config.requireDeviceBinding = true;
    config.devicePublicKey = [key]() {
        const auto publicKey = key.publicKey();
        if (publicKey.isEmpty()) throw PlankBrokerError(PlankBrokerError::DeviceKeyUnavailable);
        return publicKey;
    };
    config.deviceSigner = [key](const QByteArray& message) { return key.sign(message); };
#elif defined(Q_OS_MACOS)
    if (!PlankBroker::isPasskeyRpId(deviceHost)) return config;
    const PlankPasskeyHelper helper(macHelper);
    if (helper.available()) {
        config.devicePublicKey = [helper, deviceHost]() { return helper.devicePublicKey(deviceHost); };
        config.deviceSigner = [helper, deviceHost](const QByteArray& message) { return helper.deviceSign(deviceHost, message); };
    }
#else
    Q_UNUSED(macHelper)
#endif
    return config;
}

}
