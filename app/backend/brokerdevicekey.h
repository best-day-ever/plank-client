#pragma once

#include "plankbrokerclient.h"
#include "plankpasskey.h"

// One device-proof configuration for login, enrollment, resumed sessions and
// stream lease renewal. Passkey availability does not gate device binding.
namespace BrokerDeviceKey {

PlankBrokerClient::Config clientConfig(const QString& host, quint16 port, const QStringList& pins,
                                     std::function<bool()> proofRequired = {},
                                     const QString& macHelper = PlankPasskeyHelper::bundledProgram());

// The OS owns the persisted private key. Only public SPKI and request signatures
// leave this interface; bearer requests never create or replace a key.
class WindowsKey
{
public:
    explicit WindowsKey(QString authority) : m_Authority(std::move(authority)) {}
    QString publicKey() const;
    PlankBrokerClient::DeviceSignature sign(const QByteArray& message) const;
    static QString keyName(const QString& authority);

private:
    QString m_Authority;
};

// CNG exports an uncompressed P-256 point and returns fixed-width r || s.
// The existing broker requires SPKI and ASN.1 DER, respectively.
QByteArray p256Spki(const QByteArray& uncompressedPoint);
QByteArray ecdsaDer(const QByteArray& rawSignature);

}
