#include "plankpasskey.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>

namespace {

bool isMapping(const QString& mapping, const QString& credentialId, const QString& publicKey)
{
    QByteArray decoded;
    return PlankBroker::decodeStandardBase64(credentialId, decoded) && !decoded.isEmpty() &&
            PlankBroker::decodeStandardBase64(publicKey, decoded) && !decoded.isEmpty() &&
            mapping == QStringLiteral("passkey:%1,%2").arg(credentialId, publicKey);
}

}

PlankPasskeyHelper::PlankPasskeyHelper(QString program)
    : m_Program(std::move(program))
{
}

QString PlankPasskeyHelper::bundledProgram()
{
#ifdef Q_OS_MACOS
    if (QCoreApplication::instance() == nullptr) return QString();
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("plank-passkey"));
#else
    return QString();
#endif
}

bool PlankPasskeyHelper::available() const
{
    if (m_Program.isEmpty()) return false;
    const QFileInfo info(m_Program);
    return info.isAbsolute() && info.isFile() && info.isExecutable();
}

PlankPasskeyHelper::Result PlankPasskeyHelper::run(const QStringList& arguments, const QByteArray& input,
                                                   int timeoutMs) const
{
    Result result;
    if (!available()) return result;
    QProcess process;
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.setProgram(m_Program);
    process.setArguments(arguments);
    process.start(QIODevice::ReadWrite);
    if (!process.waitForStarted(QuickTimeoutMs)) {
        qWarning() << "plank-passkey could not be started";
        return result;
    }
    if (!input.isEmpty()) process.write(input);
    process.closeWriteChannel();
    if (!process.waitForFinished(timeoutMs)) {
        qWarning() << "plank-passkey did not finish in time";
        process.kill();
        process.waitForFinished(1000);
        return result;
    }
    const QByteArray errors = process.readAllStandardError().left(4096).trimmed();
    if (!errors.isEmpty()) {
        // Diagnostics only (no key material is ever written to stderr).
        qInfo().noquote() << QString::fromUtf8(errors);
    }
    if (process.exitStatus() != QProcess::NormalExit) return result;
    result.ran = true;
    result.exitCode = process.exitCode();
    result.output = process.readAllStandardOutput();
    if (result.output.size() > MaximumOutputBytes) {
        result.output.clear();
        result.exitCode = Failure;
    }
    return result;
}

bool PlankPasskeyHelper::parseList(const QByteArray& output, QVector<LocalKey>& keys)
{
    keys.clear();
    QJsonParseError error {};
    const QJsonDocument document = QJsonDocument::fromJson(output.trimmed(), &error);
    if (error.error != QJsonParseError::NoError || !document.isArray()) return false;
    const QJsonArray array = document.array();
    if (array.size() > 256) return false;
    QVector<LocalKey> parsed;
    for (const QJsonValue& value : array) {
        const QJsonObject object = value.toObject();
        LocalKey key;
        key.rpId = object.value(QStringLiteral("rp_id")).toString();
        key.username = object.value(QStringLiteral("username")).toString();
        key.credentialId = object.value(QStringLiteral("credential_id")).toString();
        key.mapping = object.value(QStringLiteral("mapping")).toString();
        key.created = object.value(QStringLiteral("created")).toString();
        const QString publicKey = object.value(QStringLiteral("public_key")).toString();
        if (!value.isObject() || !PlankBroker::isPasskeyRpId(key.rpId) ||
                !PlankBroker::isPasskeyUsername(key.username) ||
                !isMapping(key.mapping, key.credentialId, publicKey) ||
                !PlankBroker::isDisplayText(key.created, 64)) {
            return false;
        }
        parsed.append(key);
    }
    keys = parsed;
    return true;
}

bool PlankPasskeyHelper::parseCreated(const QByteArray& output, CreatedKey& key)
{
    key = CreatedKey();
    QJsonParseError error {};
    const QJsonDocument document = QJsonDocument::fromJson(output.trimmed(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
    const QJsonObject object = document.object();
    CreatedKey parsed;
    parsed.credentialId = object.value(QStringLiteral("credential_id")).toString();
    parsed.publicKey = object.value(QStringLiteral("public_key")).toString();
    parsed.mapping = object.value(QStringLiteral("mapping")).toString();
    if (!isMapping(parsed.mapping, parsed.credentialId, parsed.publicKey)) return false;
    key = parsed;
    return true;
}

PlankBrokerClient::PasskeyAssertResult PlankPasskeyHelper::classify(const Result& result)
{
    using R = PlankBrokerClient::PasskeyAssertResult;
    if (!result.ran) return R::Failed;
    switch (result.exitCode) {
    case Ok: return R::Signed;
    case NoMatchingKey: return R::NoMatchingKey;
    case NotConfirmed: return R::NotConfirmed;
    default: return R::Failed;
    }
}

bool PlankPasskeyHelper::list(const QString& rpId, QVector<LocalKey>& keys) const
{
    keys.clear();
    if (!PlankBroker::isPasskeyRpId(rpId)) return false;
    const Result result = run({QStringLiteral("list"), QStringLiteral("--rp"), rpId}, QByteArray(), QuickTimeoutMs);
    return result.ok() && parseList(result.output, keys);
}

bool PlankPasskeyHelper::create(const QString& rpId, const QString& username, CreatedKey& key) const
{
    key = CreatedKey();
    if (!PlankBroker::isPasskeyRpId(rpId) || !PlankBroker::isPasskeyUsername(username)) return false;
    const Result result = run({QStringLiteral("create"), QStringLiteral("--rp"), rpId,
                               QStringLiteral("--user"), username}, QByteArray(), QuickTimeoutMs);
    return result.ok() && parseCreated(result.output, key);
}

PlankPasskeyHelper::Result PlankPasskeyHelper::remove(const QString& rpId, const QString& username) const
{
    if (!PlankBroker::isPasskeyRpId(rpId) || !PlankBroker::isPasskeyUsername(username)) return Result();
    return run({QStringLiteral("delete"), QStringLiteral("--rp"), rpId, QStringLiteral("--user"), username},
               QByteArray(), QuickTimeoutMs);
}

PlankBrokerClient::PasskeyAssertResult PlankPasskeyHelper::assertion(const PlankBroker::PasskeyRequest& request,
                                                                     const QString& username,
                                                                     PlankBroker::PasskeyAssertion& assertion) const
{
    assertion = PlankBroker::PasskeyAssertion();
    if (!PlankBroker::isPasskeyUsername(username) || !PlankBroker::isPasskeyRpId(request.rpId)) {
        return PlankBrokerClient::PasskeyAssertResult::NoMatchingKey;
    }
    const Result result = run({QStringLiteral("assert"), QStringLiteral("--rp"), request.rpId,
                               QStringLiteral("--user"), username},
                              PlankBroker::passkeyHelperInput(request), AssertTimeoutMs);
    const PlankBrokerClient::PasskeyAssertResult classified = classify(result);
    if (classified != PlankBrokerClient::PasskeyAssertResult::Signed) return classified;
    if (!PlankBroker::parsePasskeyAssertion(result.output, request, assertion)) {
        qWarning() << "plank-passkey returned an assertion that does not match the challenge";
        return PlankBrokerClient::PasskeyAssertResult::Failed;
    }
    return classified;
}
