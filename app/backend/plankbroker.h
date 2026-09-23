#pragma once

// Pure, dependency-light logic for PLANK "Remote (broker)" mode: broker trust
// (SPKI pins), reply parsing, prompt answering, brokered-connect parameter
// overrides and keepalive scheduling. Header-only and free of networking so
// it is unit-testable without a broker, host or window (tests/plankbroker).
//
// Contract: bde-linux docs/plank-broker.md section 10.1 (broker API, client to
// broker over HTTPS/TLS 1.3 with a pinned SPKI SHA-256), section 10.2
// (brokered connect to a host through the broker's per-session lease) and
// section 13.3 (passkey sign-in: method "passkey", prompt style "passkey")
// and section 14.2 (device-bound sessions: "device_key", proof headers).

#include <QByteArray>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>
#include <QtGlobal>

namespace PlankBroker
{

constexpr quint16 DefaultPort = 29000;
inline QString defaultHost() { return QStringLiteral("remote.bde.run"); }

// SPKI SHA-256 of the default broker's TLS keys: current + pre-generated
// spare for rotation (self-signed ECDSA P-256). Public-key hashes only.
inline QStringList defaultPins()
{
    return {
        QStringLiteral("98819b466897df136a0bfc7d2c2c90ba74e5017a283b937f5f776e7d760f78b1"),
        QStringLiteral("ff5529586e78d22da21e3b1d0afd8aecbe695dc5d6888084489d9547e4e1dd0d"),
    };
}

constexpr int MaximumReplyBytes = 256 * 1024;
constexpr int MaximumTokenLength = 512;
constexpr int MaximumGssapiTokenLength = 64 * 1024;
constexpr int MaximumHosts = 256;
constexpr int MaximumPrompts = 8;
constexpr int DefaultRetryAfterSeconds = 30;
constexpr int MaximumRetryAfterSeconds = 3600;
constexpr int DefaultLeaseSeconds = 30;
// Passkey limits (section 13.3; the broker enforces the same caps).
constexpr int PasskeyChallengeBytes = 32;
constexpr int MaximumPasskeyCredentialIds = 64;
constexpr int MaximumPasskeyCredentialIdBytes = 1024;
constexpr int MinimumPasskeyAuthenticatorDataBytes = 37;
constexpr int MaximumPasskeyAuthenticatorDataBytes = 1024;
constexpr int MaximumPasskeySignatureBytes = 256;
inline QString defaultPasskeyRpId() { return QStringLiteral("ipa.bde.run"); }

// ---------------------------------------------------------------------------
// SHA-256 hex values (broker SPKI pins, host leaf certificate pins)
// ---------------------------------------------------------------------------

inline bool isCanonicalSha256Hex(const QString& value)
{
    if (value.size() != 64) return false;
    for (const QChar c : value) {
        const ushort u = c.unicode();
        if (!((u >= '0' && u <= '9') || (u >= 'a' && u <= 'f'))) return false;
    }
    return true;
}

// Accepts common spellings (upper case, colon- or space-separated octets) and
// returns the canonical lower-case 64-hex form, or an empty string.
inline QString normalizePin(const QString& input)
{
    QString compact;
    compact.reserve(64);
    for (const QChar c : input) {
        if (c == QLatin1Char(':') || c.isSpace()) continue;
        compact.append(c.toLower());
    }
    return isCanonicalSha256Hex(compact) ? compact : QString();
}

// Splits a user-entered pin list (newline, comma or semicolon separated).
// Invalid entries are dropped and reported through `rejected`.
inline QStringList parsePinList(const QString& text, QStringList* rejected = nullptr)
{
    QStringList pins;
    const QStringList parts = text.split(QRegularExpression(QStringLiteral("[,;\\n\\r]+")),
                                         Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        const QString trimmed = part.trimmed();
        if (trimmed.isEmpty()) continue;
        const QString pin = normalizePin(trimmed);
        if (pin.isEmpty()) {
            if (rejected != nullptr) rejected->append(trimmed);
            continue;
        }
        if (!pins.contains(pin)) pins.append(pin);
    }
    return pins;
}

inline QStringList normalizePins(const QStringList& input)
{
    return parsePinList(input.join(QLatin1Char('\n')));
}

// ---------------------------------------------------------------------------
// Minimal DER walk: Certificate -> TBSCertificate -> SubjectPublicKeyInfo.
// Backend independent (OpenSSL/SecureTransport differ in QSslKey::toDer()).
// ---------------------------------------------------------------------------

namespace detail
{
struct Tlv {
    quint8 tag = 0;
    int headerLength = 0;
    int contentLength = 0;
    int totalLength() const { return headerLength + contentLength; }
};

inline bool readTlv(const QByteArray& data, int offset, int end, Tlv& tlv)
{
    if (offset < 0 || offset + 2 > end || end > data.size()) return false;
    tlv.tag = static_cast<quint8>(data.at(offset));
    // Multi-byte tag numbers never occur in the X.509 fields we walk.
    if ((tlv.tag & 0x1f) == 0x1f) return false;
    const quint8 first = static_cast<quint8>(data.at(offset + 1));
    int length = 0;
    int header = 2;
    if (first < 0x80) {
        length = first;
    } else {
        const int octets = first & 0x7f;
        if (octets == 0 || octets > 3 || offset + 2 + octets > end) return false;
        for (int i = 0; i < octets; ++i) {
            length = (length << 8) | static_cast<quint8>(data.at(offset + 2 + i));
        }
        // DER requires the shortest length form.
        if (length < 0x80 || (octets > 1 && static_cast<quint8>(data.at(offset + 2)) == 0)) return false;
        header += octets;
    }
    if (length < 0 || offset + header + length > end) return false;
    tlv.headerLength = header;
    tlv.contentLength = length;
    return true;
}
}

// Returns the complete DER TLV of the certificate's SubjectPublicKeyInfo, or
// an empty array for anything that is not a well-formed X.509 certificate.
inline QByteArray subjectPublicKeyInfoDer(const QByteArray& certificateDer)
{
    using detail::Tlv;
    using detail::readTlv;
    Tlv certificate;
    if (!readTlv(certificateDer, 0, certificateDer.size(), certificate) ||
            certificate.tag != 0x30 || certificate.totalLength() != certificateDer.size()) {
        return {};
    }
    Tlv tbs;
    const int tbsOffset = certificate.headerLength;
    if (!readTlv(certificateDer, tbsOffset, certificateDer.size(), tbs) || tbs.tag != 0x30) return {};
    int offset = tbsOffset + tbs.headerLength;
    const int tbsEnd = tbsOffset + tbs.totalLength();

    Tlv field;
    if (!readTlv(certificateDer, offset, tbsEnd, field)) return {};
    if (field.tag == 0xa0) { // [0] EXPLICIT version
        offset += field.totalLength();
        if (!readTlv(certificateDer, offset, tbsEnd, field)) return {};
    }
    // serialNumber INTEGER, signature SEQ, issuer SEQ, validity SEQ, subject SEQ
    static const quint8 expected[] = { 0x02, 0x30, 0x30, 0x30, 0x30 };
    for (const quint8 tag : expected) {
        if (field.tag != tag) return {};
        offset += field.totalLength();
        if (!readTlv(certificateDer, offset, tbsEnd, field)) return {};
    }
    if (field.tag != 0x30) return {};
    return certificateDer.mid(offset, field.totalLength());
}

inline QString spkiSha256Hex(const QByteArray& certificateDer)
{
    const QByteArray spki = subjectPublicKeyInfoDer(certificateDer);
    if (spki.isEmpty()) return {};
    return QString::fromLatin1(QCryptographicHash::hash(spki, QCryptographicHash::Sha256).toHex());
}

// Broker trust decision: the leaf's SPKI SHA-256 must equal one configured
// pin (current or spare). No WebPKI, no host-name check. An empty or wholly
// invalid pin list never matches.
inline bool spkiPinMatches(const QByteArray& certificateDer, const QStringList& pins)
{
    const QString actual = spkiSha256Hex(certificateDer);
    if (actual.isEmpty()) return false;
    for (const QString& pin : pins) {
        if (normalizePin(pin) == actual) return true;
    }
    return false;
}

// Brokered host trust decision (section 10.2 step 1): SHA-256 over the host
// leaf certificate's DER must equal the broker-supplied host_cert_sha256.
inline bool hostLeafMatches(const QByteArray& leafDer, const QString& hostCertSha256)
{
    if (leafDer.isEmpty() || !isCanonicalSha256Hex(hostCertSha256)) return false;
    return QString::fromLatin1(QCryptographicHash::hash(leafDer, QCryptographicHash::Sha256).toHex()) ==
            hostCertSha256;
}

// ---------------------------------------------------------------------------
// Replies (section 10.1)
// ---------------------------------------------------------------------------

enum class ReplyKind {
    Challenge,
    Authenticated,
    Denied,
    RateLimited,
    SessionExpired,
    Malformed,
};

// Section 13.3: the "passkey" prompt's WebAuthn-style request. Values are
// kept exactly as sent (standard base64); they are validated on parse.
struct PasskeyRequest {
    QString rpId;
    QStringList credentialIds;
    bool userVerification = true;
    QString challenge;
};

struct Prompt {
    QString id;
    QString style; // "secret", "otp", "text", "info" or "passkey"
    QString text;
    PasskeyRequest passkey {}; // style "passkey" only
};

// What the helper returns and /v1/auth/respond carries (standard base64).
struct PasskeyAssertion {
    QString credentialId;
    QString authenticatorData;
    QString signature;
};

// Sign-in method sent with /v1/auth/start. PasswordOtp sends no "method"
// member at all, so the password + code request is unchanged.
enum class AuthMethod { PasswordOtp, Passkey };

struct AuthReply {
    ReplyKind kind = ReplyKind::Malformed;
    QString conversationId;
    QVector<Prompt> prompts;
    QString sessionToken;
    int expiresIn = 0;
    QString username;
    int retryAfter = 0;
    bool deviceBound = false; // section 14.2; absent means an unbound session
    bool passkeySetupAvailable = false; // fresh OTP login; absent on older brokers
};

struct Host {
    QString id;
    QString name;
    bool online = false;
    QString inUseBy;
    bool connectable = false;
    QString reason;
};

// Route of a brokered connect: Relay = endpoint:port is a broker lease,
// Direct = endpoint:port is the workstation itself (client on the office LAN).
enum class Route { Relay, Direct };

struct Lease {
    Route route = Route::Relay;
    QString endpoint;
    quint16 port = 0;
    QString hostCertSha256;
    QString username;
    QString gssapiToken;
    int expiresIn = DefaultLeaseSeconds;
};

inline bool isPrintableAscii(const QString& value, int maximumLength)
{
    if (value.isEmpty() || value.size() > maximumLength) return false;
    for (const QChar c : value) {
        if (c.unicode() < 0x21 || c.unicode() > 0x7e) return false;
    }
    return true;
}

inline bool isDisplayText(const QString& value, int maximumLength)
{
    if (value.size() > maximumLength) return false;
    for (const QChar c : value) {
        if (c.category() == QChar::Other_Control) return false;
    }
    return true;
}

inline bool isHostId(const QString& value)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9](?:[A-Za-z0-9.-]{0,251}[A-Za-z0-9])?$"));
    return pattern.match(value).hasMatch();
}

inline bool isEndpointName(const QString& value)
{
    // DNS name or IPv4 literal; the client dials exactly what it used to
    // reach the broker, so nothing more exotic is expected.
    return isHostId(value);
}

inline int retryAfterFrom(const QJsonObject& object)
{
    const QJsonValue value = object.value(QStringLiteral("retry_after"));
    if (!value.isDouble()) return DefaultRetryAfterSeconds;
    const double seconds = value.toDouble();
    if (!(seconds >= 1)) return 1;
    return static_cast<int>(qMin<double>(seconds, MaximumRetryAfterSeconds));
}

inline bool parseObject(const QByteArray& body, QJsonObject& object)
{
    if (body.size() > MaximumReplyBytes) return false;
    QJsonParseError error {};
    const QJsonDocument document = QJsonDocument::fromJson(body, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
    object = document.object();
    return true;
}

// Strict standard base64 (padding required, canonical, no whitespace).
inline bool decodeStandardBase64(const QString& value, QByteArray& decoded)
{
    decoded.clear();
    if (value.isEmpty() || value.size() % 4 != 0) return false;
    for (const QChar c : value) {
        const ushort u = c.unicode();
        if (!((u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') ||
              u == '+' || u == '/' || u == '=')) {
            return false;
        }
    }
    const auto result = QByteArray::fromBase64Encoding(value.toLatin1(),
                                                       QByteArray::AbortOnBase64DecodingErrors);
    if (!result || result.decoded.toBase64() != value.toLatin1()) return false;
    decoded = result.decoded;
    return true;
}

// Relying party id: a plain lower-case DNS host name (not an IP literal);
// the same rule as the plank-passkey helper.
inline bool isPasskeyRpId(const QString& value)
{
    if (value.isEmpty() || value.size() > 253) return false;
    const QStringList labels = value.split(QLatin1Char('.'));
    for (const QString& label : labels) {
        if (label.isEmpty() || label.size() > 63) return false;
        for (int i = 0; i < label.size(); ++i) {
            const ushort u = label.at(i).unicode();
            const bool alnum = (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9');
            const bool hyphen = u == '-' && i != 0 && i != label.size() - 1;
            if (!alnum && !hyphen) return false;
        }
    }
    for (const QChar c : labels.last()) {
        if (c.unicode() >= 'a' && c.unicode() <= 'z') return true;
    }
    return false;
}

// User names the passkey helper stores keys under (FreeIPA: lower case).
inline QString normalizePasskeyUsername(const QString& username)
{
    return username.trimmed().toLower();
}

inline bool isPasskeyUsername(const QString& value)
{
    static const QRegularExpression pattern(QStringLiteral("^[a-z0-9_][a-z0-9_.-]{0,63}$"));
    return pattern.match(value).hasMatch();
}

inline bool parsePasskeyRequest(const QJsonValue& value, PasskeyRequest& request)
{
    request = PasskeyRequest();
    if (!value.isObject()) return false;
    const QJsonObject object = value.toObject();
    PasskeyRequest parsed;
    parsed.rpId = object.value(QStringLiteral("rp_id")).toString();
    parsed.challenge = object.value(QStringLiteral("challenge")).toString();
    const QJsonValue ids = object.value(QStringLiteral("credential_ids"));
    const QJsonValue verification = object.value(QStringLiteral("user_verification"));
    if (!isPasskeyRpId(parsed.rpId) || !ids.isArray() ||
            !(verification.isBool() || verification.isUndefined())) {
        return false;
    }
    QByteArray decoded;
    if (!decodeStandardBase64(parsed.challenge, decoded) || decoded.size() != PasskeyChallengeBytes) return false;
    const QJsonArray idArray = ids.toArray();
    if (idArray.isEmpty() || idArray.size() > MaximumPasskeyCredentialIds) return false;
    for (const QJsonValue& id : idArray) {
        const QString text = id.toString();
        if (!id.isString() || !decodeStandardBase64(text, decoded) ||
                decoded.size() > MaximumPasskeyCredentialIdBytes) {
            return false;
        }
        parsed.credentialIds.append(text);
    }
    parsed.userVerification = verification.isUndefined() || verification.toBool();
    request = parsed;
    return true;
}

// The object handed to `plank-passkey assert` on stdin.
inline QByteArray passkeyHelperInput(const PasskeyRequest& request)
{
    const QJsonObject object {
        {QStringLiteral("rp_id"), request.rpId},
        {QStringLiteral("credential_ids"), QJsonArray::fromStringList(request.credentialIds)},
        {QStringLiteral("challenge"), request.challenge},
        {QStringLiteral("user_verification"), request.userVerification},
    };
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

// Validates the helper's stdout against the request before anything is sent:
// an allowed credential id, authData for this rp with UP set, bounded sizes.
inline bool parsePasskeyAssertion(const QByteArray& output, const PasskeyRequest& request,
                                  PasskeyAssertion& assertion)
{
    assertion = PasskeyAssertion();
    QJsonObject object;
    if (output.size() > 16 * 1024) return false;
    QJsonParseError error {};
    const QJsonDocument document = QJsonDocument::fromJson(output.trimmed(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
    object = document.object();
    PasskeyAssertion parsed;
    parsed.credentialId = object.value(QStringLiteral("credential_id")).toString();
    parsed.authenticatorData = object.value(QStringLiteral("authenticator_data")).toString();
    parsed.signature = object.value(QStringLiteral("signature")).toString();
    QByteArray credentialId;
    QByteArray authData;
    QByteArray signature;
    if (!request.credentialIds.contains(parsed.credentialId) ||
            !decodeStandardBase64(parsed.credentialId, credentialId) ||
            !decodeStandardBase64(parsed.authenticatorData, authData) ||
            !decodeStandardBase64(parsed.signature, signature) ||
            authData.size() < MinimumPasskeyAuthenticatorDataBytes ||
            authData.size() > MaximumPasskeyAuthenticatorDataBytes ||
            signature.isEmpty() || signature.size() > MaximumPasskeySignatureBytes ||
            authData.left(32) != QCryptographicHash::hash(request.rpId.toUtf8(), QCryptographicHash::Sha256) ||
            (static_cast<quint8>(authData.at(32)) & 0x01) == 0) {
        return false;
    }
    assertion = parsed;
    return true;
}

// The single passkey prompt of a challenge, or nullptr (e.g. an older broker
// that ignored "method" and asked for password + code).
inline const Prompt* passkeyPrompt(const QVector<Prompt>& prompts)
{
    if (prompts.size() != 1 || prompts.at(0).style != QLatin1String("passkey")) return nullptr;
    return &prompts.at(0);
}

// {"conversation_id", "prompts":[...]} of a challenge (section 10.1; also
// the enrolment start reply). Sets reply.kind to Challenge only when valid.
inline bool parseChallenge(const QJsonObject& object, AuthReply& reply)
{
    const QString conversationId = object.value(QStringLiteral("conversation_id")).toString();
    const QJsonValue promptsValue = object.value(QStringLiteral("prompts"));
    if (!isPrintableAscii(conversationId, MaximumTokenLength) || !promptsValue.isArray()) return false;
    const QJsonArray prompts = promptsValue.toArray();
    if (prompts.isEmpty() || prompts.size() > MaximumPrompts) return false;
    QVector<Prompt> parsed;
    for (const QJsonValue& value : prompts) {
        if (!value.isObject()) return false;
        const QJsonObject promptObject = value.toObject();
        Prompt prompt;
        prompt.id = promptObject.value(QStringLiteral("id")).toString();
        prompt.style = promptObject.value(QStringLiteral("style")).toString();
        prompt.text = promptObject.value(QStringLiteral("text")).toString();
        if (!isPrintableAscii(prompt.id, 64) || !isDisplayText(prompt.text, 200) ||
                (prompt.style != QLatin1String("secret") && prompt.style != QLatin1String("otp") &&
                 prompt.style != QLatin1String("text") && prompt.style != QLatin1String("info") &&
                 prompt.style != QLatin1String("passkey"))) {
            return false;
        }
        if (prompt.style == QLatin1String("passkey") &&
                !parsePasskeyRequest(promptObject.value(QStringLiteral("passkey")), prompt.passkey)) {
            return false;
        }
        parsed.append(prompt);
    }
    reply.prompts = parsed;
    reply.conversationId = conversationId;
    reply.kind = ReplyKind::Challenge;
    return true;
}

// The fields of a successful sign-in ({"state":"authenticated",...}); shared
// by /v1/auth/respond and the enrolment reply that ends in a session. Sets
// reply.kind to Authenticated only when every field is well-formed.
inline bool parseAuthenticated(const QJsonObject& object, AuthReply& reply)
{
    const QString token = object.value(QStringLiteral("session_token")).toString();
    const QJsonValue expires = object.value(QStringLiteral("expires_in"));
    const QString username = object.value(QStringLiteral("username")).toString();
    if (!isPrintableAscii(token, MaximumTokenLength) ||
            (!expires.isUndefined() && (!expires.isDouble() || expires.toDouble() < 1)) ||
            !isDisplayText(username, 255)) {
        reply.prompts.clear();
        return false;
    }
    reply.sessionToken = token;
    reply.expiresIn = expires.isUndefined() ? 0 : expires.toInt();
    reply.username = username;
    // Informational only: tolerated when absent or not a boolean.
    reply.deviceBound = object.value(QStringLiteral("device_bound")).toBool(false);
    reply.passkeySetupAvailable = object.value(QStringLiteral("passkey_setup_available")).toBool(false);
    reply.kind = ReplyKind::Authenticated;
    return true;
}

// Maps HTTP status + body of /v1/auth/start and /v1/auth/respond. Every auth
// failure is HTTP 200 {"state":"denied"}; rate limiting is HTTP 429.
inline AuthReply parseAuthReply(int httpStatus, const QByteArray& body)
{
    AuthReply reply;
    QJsonObject object;
    const bool parsed = parseObject(body, object);
    if (httpStatus == 429) {
        reply.kind = ReplyKind::RateLimited;
        reply.retryAfter = parsed ? retryAfterFrom(object) : DefaultRetryAfterSeconds;
        return reply;
    }
    if (httpStatus != 200 || !parsed) return reply; // Malformed

    const QString state = object.value(QStringLiteral("state")).toString();
    if (state == QLatin1String("denied")) {
        reply.kind = ReplyKind::Denied;
        return reply;
    }
    if (state == QLatin1String("challenge")) {
        parseChallenge(object, reply);
        return reply;
    }
    if (state == QLatin1String("authenticated")) {
        parseAuthenticated(object, reply);
        return reply;
    }
    return reply;
}

inline bool isValidOtp(const QString& code)
{
    if (code.size() != 6) return false;
    for (const QChar c : code) {
        if (c.unicode() < '0' || c.unicode() > '9') return false;
    }
    return true;
}

// Answers the broker prompts in order: secret -> password, otp -> 6-digit
// code, text -> username, info -> "". Each secret/otp prompt must be
// answerable; an unknown or unanswerable prompt fails closed.
inline bool buildResponses(const QVector<Prompt>& prompts, const QString& username,
                           const QString& password, const QString& otp, QJsonArray& responses)
{
    responses = QJsonArray();
    for (const Prompt& prompt : prompts) {
        if (prompt.style == QLatin1String("secret")) {
            if (password.isEmpty()) return false;
            responses.append(password);
        } else if (prompt.style == QLatin1String("otp")) {
            if (!isValidOtp(otp)) return false;
            responses.append(otp);
        } else if (prompt.style == QLatin1String("text")) {
            if (username.isEmpty()) return false;
            responses.append(username);
        } else if (prompt.style == QLatin1String("info")) {
            responses.append(QString());
        } else {
            responses = QJsonArray();
            return false;
        }
    }
    return !responses.isEmpty();
}

inline bool parseHosts(const QByteArray& body, QVector<Host>& hosts)
{
    hosts.clear();
    QJsonObject object;
    if (!parseObject(body, object)) return false;
    const QJsonValue list = object.value(QStringLiteral("hosts"));
    if (!list.isArray()) return false;
    const QJsonArray array = list.toArray();
    if (array.size() > MaximumHosts) return false;
    QVector<Host> parsed;
    for (const QJsonValue& value : array) {
        if (!value.isObject()) return false;
        const QJsonObject entry = value.toObject();
        Host host;
        host.id = entry.value(QStringLiteral("id")).toString();
        const QJsonValue name = entry.value(QStringLiteral("name"));
        const QJsonValue online = entry.value(QStringLiteral("online"));
        const QJsonValue inUseBy = entry.value(QStringLiteral("in_use_by"));
        const QJsonValue connectable = entry.value(QStringLiteral("connectable"));
        const QJsonValue reason = entry.value(QStringLiteral("reason"));
        if (!isHostId(host.id) || !online.isBool() || !connectable.isBool() ||
                !(name.isString() || name.isUndefined() || name.isNull()) ||
                !(inUseBy.isString() || inUseBy.isNull() || inUseBy.isUndefined()) ||
                !(reason.isString() || reason.isNull() || reason.isUndefined())) {
            return false;
        }
        host.name = name.toString();
        if (host.name.isEmpty()) host.name = host.id;
        host.online = online.toBool();
        host.inUseBy = inUseBy.toString();
        host.connectable = connectable.toBool();
        host.reason = reason.toString();
        if (!isDisplayText(host.name, 253) || !isDisplayText(host.inUseBy, 255) ||
                !isDisplayText(host.reason, 200)) {
            return false;
        }
        parsed.append(host);
    }
    hosts = parsed;
    return true;
}

inline bool parseLease(const QByteArray& body, Lease& lease)
{
    lease = Lease();
    QJsonObject object;
    if (!parseObject(body, object)) return false;
    Lease parsed;
    parsed.endpoint = object.value(QStringLiteral("endpoint")).toString();
    const QJsonValue port = object.value(QStringLiteral("port"));
    parsed.hostCertSha256 = object.value(QStringLiteral("host_cert_sha256")).toString();
    parsed.username = object.value(QStringLiteral("username")).toString();
    parsed.gssapiToken = object.value(QStringLiteral("gssapi_token")).toString();
    const QJsonValue expires = object.value(QStringLiteral("expires_in"));
    // Brokers before the direct route omit "route": everything was relayed.
    const QJsonValue route = object.value(QStringLiteral("route"));
    if (route == QJsonValue(QStringLiteral("direct"))) {
        parsed.route = Route::Direct;
    } else if (!route.isUndefined() && route != QJsonValue(QStringLiteral("relay"))) {
        return false;
    }
    if (!isEndpointName(parsed.endpoint) || !port.isDouble() ||
            port.toDouble() != static_cast<double>(port.toInt()) ||
            port.toInt() < 1 || port.toInt() > 65535 ||
            !isCanonicalSha256Hex(parsed.hostCertSha256) ||
            !isPrintableAscii(parsed.username, 255) ||
            parsed.gssapiToken.isEmpty() || parsed.gssapiToken.size() > MaximumGssapiTokenLength ||
            (!expires.isUndefined() && (!expires.isDouble() || expires.toDouble() < 1))) {
        return false;
    }
    const QByteArray encoded = parsed.gssapiToken.toLatin1();
    const auto decoded = QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded || decoded.decoded.isEmpty()) return false;
    parsed.port = static_cast<quint16>(port.toInt());
    if (!expires.isUndefined()) parsed.expiresIn = expires.toInt();
    lease = parsed;
    return true;
}

// Bearer-authenticated calls (hosts, connect, keepalive, logout).
enum class BearerStatus { Ok, SessionExpired, RateLimited, Unavailable, Denied, Malformed };

inline BearerStatus classifyBearerStatus(int httpStatus)
{
    if (httpStatus >= 200 && httpStatus < 300) return BearerStatus::Ok;
    if (httpStatus == 401) return BearerStatus::SessionExpired;
    if (httpStatus == 429) return BearerStatus::RateLimited;
    if (httpStatus == 409) return BearerStatus::Unavailable;
    if (httpStatus == 403 || httpStatus == 404 || httpStatus == 410) return BearerStatus::Denied;
    return BearerStatus::Malformed;
}

inline bool isTransientOfflineResponse(int httpStatus, const QByteArray& body)
{
    if (httpStatus != 409) return false;
    QJsonObject object;
    return parseObject(body, object) &&
            object.value(QStringLiteral("state")) == QLatin1String("unavailable") &&
            object.value(QStringLiteral("reason")) == QLatin1String("offline");
}

// Path for /v1/hosts/{id}/<action>, with the id as one encoded path segment.
inline QString hostActionPath(const QString& hostId, const QString& action)
{
    return QStringLiteral("/v1/hosts/") + QString::fromLatin1(QUrl::toPercentEncoding(hostId)) +
            QLatin1Char('/') + action;
}

// ---------------------------------------------------------------------------
// Device-bound sessions (section 14.2)
// ---------------------------------------------------------------------------

// The broker accepts P-256 SPKI DER keys of at most 200 bytes (91 in practice).
constexpr int MaximumDevicePublicKeyBytes = 200;
// A DER ECDSA P-256 signature is at most 72 bytes.
constexpr int MaximumDeviceSignatureBytes = 72;
constexpr int MinimumDeviceSignatureBytes = 8;
inline QByteArray deviceTimeHeader() { return QByteArrayLiteral("X-Plank-Device-Time"); }
inline QByteArray deviceProofHeader() { return QByteArrayLiteral("X-Plank-Device-Proof"); }

// "device_key" for /v1/auth/start: standard base64 of a DER SubjectPublicKeyInfo.
inline bool isDevicePublicKey(const QString& value)
{
    QByteArray decoded;
    return decodeStandardBase64(value, decoded) && !decoded.isEmpty() &&
            decoded.size() <= MaximumDevicePublicKeyBytes && decoded.at(0) == '\x30';
}

// X-Plank-Device-Proof value: standard base64 of a DER ECDSA signature.
inline bool isDeviceSignature(const QString& value)
{
    QByteArray decoded;
    return decodeStandardBase64(value, decoded) && decoded.size() >= MinimumDeviceSignatureBytes &&
            decoded.size() <= MaximumDeviceSignatureBytes && decoded.at(0) == '\x30';
}

// The UTF-8 message a device proof signs:
//   plank-device-proof-v1\n<METHOD>\n<path>\n<time>\n<hex sha256(token)>\n<hex sha256(body)>
// METHOD upper case, path exactly as sent without the query, time in unix
// seconds, body the exact request bytes (empty for GET), hex lower case.
inline QByteArray deviceProofMessage(const QByteArray& method, const QByteArray& path, qint64 unixTime,
                                     const QString& sessionToken, const QByteArray& body)
{
    const int query = path.indexOf('?');
    QByteArray message("plank-device-proof-v1\n");
    message += method.toUpper();
    message += '\n';
    message += query < 0 ? path : path.left(query);
    message += '\n';
    message += QByteArray::number(unixTime);
    message += '\n';
    message += QCryptographicHash::hash(sessionToken.toUtf8(), QCryptographicHash::Sha256).toHex();
    message += '\n';
    message += QCryptographicHash::hash(body, QCryptographicHash::Sha256).toHex();
    return message;
}

// ---------------------------------------------------------------------------
// Brokered connect (section 10.2)
// ---------------------------------------------------------------------------

// Where QUIC goes and which certificate it must present. In brokered mode the
// media transport always targets the leased endpoint:port and ignores the
// port the host advertises in its launch reply (28989 behind the broker's
// DNAT is not reachable directly). The host's QUIC certificate must be the
// same leaf that the broker pinned for HTTPS.
struct TransportTarget {
    quint16 port = 0;
    QString certificateSha256;
};

inline bool resolveTransportTarget(const QString& brokerPin, quint16 controlPort,
                                   quint16 launchPort, const QString& launchCertificate,
                                   TransportTarget& target)
{
    target = TransportTarget();
    if (brokerPin.isEmpty()) {
        if (launchPort == 0 || !isCanonicalSha256Hex(launchCertificate)) return false;
        target.port = launchPort;
        target.certificateSha256 = launchCertificate;
        return true;
    }
    if (!isCanonicalSha256Hex(brokerPin) || controlPort == 0) return false;
    // The host reports its transport leaf digest in upper-case hex (Sunshine's
    // util::hex_vec); the broker pin is lower-case. Same digest, same leaf.
    if (!launchCertificate.isEmpty() &&
            launchCertificate.compare(brokerPin, Qt::CaseInsensitive) != 0) {
        return false;
    }
    target.port = controlPort;
    target.certificateSha256 = brokerPin;
    return true;
}

// Host reply to POST /plank/auth/start {"username","gssapi_token"} (section
// 10.2 step 2). Same shape as the password path, but a challenge is never
// answered: brokered mode must not fall back to a password conversation.
enum class AdmissionResult { Authenticated, Denied, Busy, Malformed };

inline AdmissionResult parseHostAdmission(const QJsonObject& reply, QString& sessionToken)
{
    sessionToken.clear();
    const QString state = reply.value(QStringLiteral("state")).toString();
    if (state == QLatin1String("authenticated")) {
        const QString token = reply.value(QStringLiteral("session_token")).toString();
        if (!isPrintableAscii(token, MaximumTokenLength)) return AdmissionResult::Malformed;
        sessionToken = token;
        return AdmissionResult::Authenticated;
    }
    if (state == QLatin1String("denied")) return AdmissionResult::Denied;
    if (state == QLatin1String("busy")) return AdmissionResult::Busy;
    return AdmissionResult::Malformed;
}

// ---------------------------------------------------------------------------
// Keepalive (section 10.2 step 4: at least every 60 s while streaming)
// ---------------------------------------------------------------------------

class KeepaliveSchedule
{
public:
    static constexpr qint64 IntervalMs = 30000;   // regular cadence (30-45 s window)
    static constexpr qint64 RetryMs = 10000;      // after a transient failure
    static constexpr qint64 LeaseWindowMs = 60000;
    static constexpr qint64 SafetyMarginMs = 5000;

    void start(qint64 nowMs) { m_LastSuccessMs = nowMs; m_Failures = 0; m_Active = true; }
    void stop() { m_Active = false; }
    bool active() const { return m_Active; }
    int failures() const { return m_Failures; }

    void recordSuccess(qint64 nowMs) { m_LastSuccessMs = nowMs; m_Failures = 0; }
    void recordFailure() { ++m_Failures; }

    // Delay until the next keepalive, or -1 once the lease must be assumed
    // lost (no success within the lease window) or the schedule is stopped.
    qint64 nextDelayMs(qint64 nowMs) const
    {
        if (!m_Active) return -1;
        const qint64 deadline = m_LastSuccessMs + LeaseWindowMs - SafetyMarginMs;
        if (m_Failures == 0) {
            return qMax<qint64>(0, qMin(m_LastSuccessMs + IntervalMs, deadline) - nowMs);
        }
        const qint64 remaining = deadline - nowMs;
        if (remaining <= 0) return -1;
        return qMin(RetryMs, remaining);
    }

private:
    qint64 m_LastSuccessMs = 0;
    int m_Failures = 0;
    bool m_Active = false;
};

} // namespace PlankBroker
