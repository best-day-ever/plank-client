#include "brokersessionstore.h"

#ifdef Q_OS_DARWIN
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#endif

#ifdef Q_OS_WIN
#include <windows.h>
#include <dpapi.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#endif

namespace BrokerSessionStore {

#ifdef Q_OS_DARWIN

namespace {

// Internal identifier; unchanged by product renames so saved sessions survive.
const char* const Service = "la.instinctual.PLANK.Client.broker-session";

struct CfRelease
{
    CFTypeRef ref = nullptr;
    ~CfRelease() { if (ref != nullptr) CFRelease(ref); }
};

CFStringRef cfString(const QString& value)
{
    const QByteArray utf8 = value.toUtf8();
    return CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(utf8.constData()),
                                   utf8.size(), kCFStringEncodingUTF8, false);
}

CFMutableDictionaryRef baseQuery(const QString& brokerAddress, CfRelease& service, CfRelease& account)
{
    service.ref = cfString(QString::fromLatin1(Service));
    account.ref = cfString(brokerAddress);
    CFMutableDictionaryRef query = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                                                             &kCFTypeDictionaryKeyCallBacks,
                                                             &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query, kSecAttrService, service.ref);
    CFDictionarySetValue(query, kSecAttrAccount, account.ref);
    return query;
}

}

bool isAvailable()
{
    return true;
}

bool save(const QString& brokerAddress, const QString& username, const QString& token)
{
    if (brokerAddress.isEmpty()) return false;
    QByteArray payload = encode(username, token);
    CfRelease service, account, query, data, attributes;
    query.ref = baseQuery(brokerAddress, service, account);
    data.ref = CFDataCreate(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(payload.constData()), payload.size());
    payload.fill('\0');

    // Replace any previous session for this broker.
    SecItemDelete(static_cast<CFDictionaryRef>(query.ref));
    CFMutableDictionaryRef add = CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0,
                                                               static_cast<CFDictionaryRef>(query.ref));
    attributes.ref = add;
    CFDictionarySetValue(add, kSecValueData, data.ref);
    CFDictionarySetValue(add, kSecAttrAccessible, kSecAttrAccessibleWhenUnlockedThisDeviceOnly);
    CFDictionarySetValue(add, kSecAttrSynchronizable, kCFBooleanFalse);
    CFDictionarySetValue(add, kSecAttrLabel, CFSTR("Remote access session"));
    return SecItemAdd(add, nullptr) == errSecSuccess;
}

bool load(const QString& brokerAddress, Saved& saved)
{
    saved = Saved();
    if (brokerAddress.isEmpty()) return false;
    CfRelease service, account, query, result;
    query.ref = baseQuery(brokerAddress, service, account);
    CFMutableDictionaryRef q = static_cast<CFMutableDictionaryRef>(const_cast<void*>(query.ref));
    CFDictionarySetValue(q, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(q, kSecMatchLimit, kSecMatchLimitOne);
    if (SecItemCopyMatching(q, &result.ref) != errSecSuccess || result.ref == nullptr ||
            CFGetTypeID(result.ref) != CFDataGetTypeID()) {
        return false;
    }
    CFDataRef data = static_cast<CFDataRef>(result.ref);
    QByteArray payload(reinterpret_cast<const char*>(CFDataGetBytePtr(data)), int(CFDataGetLength(data)));
    const bool ok = decode(payload, saved);
    payload.fill('\0');
    if (!ok) clear(brokerAddress);
    return ok;
}

void clear(const QString& brokerAddress)
{
    if (brokerAddress.isEmpty()) return;
    CfRelease service, account, query;
    query.ref = baseQuery(brokerAddress, service, account);
    SecItemDelete(static_cast<CFDictionaryRef>(query.ref));
}

#elif defined(Q_OS_WIN)

namespace {

QString sessionPath(const QString& brokerAddress)
{
    const QString root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (root.isEmpty() || brokerAddress.isEmpty()) return QString();
    const QByteArray digest = QCryptographicHash::hash(brokerAddress.toUtf8(),
                                                       QCryptographicHash::Sha256).toHex();
    return QDir(root).filePath(QStringLiteral("broker-sessions/") + QString::fromLatin1(digest) +
                               QStringLiteral(".dat"));
}

DATA_BLOB dataBlob(QByteArray& data)
{
    return {static_cast<DWORD>(data.size()), reinterpret_cast<BYTE*>(data.data())};
}

}

bool isAvailable()
{
    return !QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation).isEmpty();
}

bool save(const QString& brokerAddress, const QString& username, const QString& token)
{
    const QString path = sessionPath(brokerAddress);
    if (path.isEmpty() || username.isEmpty() || token.isEmpty()) return false;
    QByteArray plaintext = encode(username, token);
    if (plaintext.size() > 4096) {
        plaintext.fill('\0');
        return false;
    }
    QByteArray entropy = brokerAddress.toUtf8();
    DATA_BLOB input = dataBlob(plaintext);
    DATA_BLOB optionalEntropy = dataBlob(entropy);
    DATA_BLOB protectedData {};
    const BOOL protectedOk = CryptProtectData(&input, L"BDE fernweh session", &optionalEntropy,
                                               nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                                               &protectedData);
    plaintext.fill('\0');
    if (!protectedOk || protectedData.pbData == nullptr) {
        if (protectedData.pbData != nullptr) LocalFree(protectedData.pbData);
        return false;
    }

    const bool directoryOk = QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    const bool opened = directoryOk && file.open(QIODevice::WriteOnly);
    const bool written = opened && file.write(reinterpret_cast<const char*>(protectedData.pbData),
                                               protectedData.cbData) == protectedData.cbData;
    const bool committed = written && file.commit();
    SecureZeroMemory(protectedData.pbData, protectedData.cbData);
    LocalFree(protectedData.pbData);
    return committed;
}

bool load(const QString& brokerAddress, Saved& saved)
{
    saved = Saved();
    const QString path = sessionPath(brokerAddress);
    if (path.isEmpty()) return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > 16384) return false;
    QByteArray protectedBytes = file.readAll();
    QByteArray entropy = brokerAddress.toUtf8();
    DATA_BLOB input = dataBlob(protectedBytes);
    DATA_BLOB optionalEntropy = dataBlob(entropy);
    DATA_BLOB plaintext {};
    const BOOL unprotected = CryptUnprotectData(&input, nullptr, &optionalEntropy,
                                                 nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                                                 &plaintext);
    if (!unprotected || plaintext.pbData == nullptr) {
        if (plaintext.pbData != nullptr) LocalFree(plaintext.pbData);
        clear(brokerAddress);
        return false;
    }
    QByteArray decoded(reinterpret_cast<const char*>(plaintext.pbData), plaintext.cbData);
    const bool valid = decode(decoded, saved);
    decoded.fill('\0');
    SecureZeroMemory(plaintext.pbData, plaintext.cbData);
    LocalFree(plaintext.pbData);
    if (!valid) clear(brokerAddress);
    return valid;
}

void clear(const QString& brokerAddress)
{
    const QString path = sessionPath(brokerAddress);
    if (!path.isEmpty()) QFile::remove(path);
}

#else

bool isAvailable() { return false; }
bool save(const QString&, const QString&, const QString&) { return false; }
bool load(const QString&, Saved& saved) { saved = Saved(); return false; }
void clear(const QString&) {}

#endif

}
