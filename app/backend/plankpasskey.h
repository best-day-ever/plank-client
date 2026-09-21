#pragma once

#include "plankbroker.h"
#include "plankbrokerclient.h"

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

// Runs the bundled `plank-passkey` helper (bde-linux docs/plank-broker.md
// section 13.4): a Secure Enclave P-256 key per (relying party, user) that
// Touch ID or the Mac password unlocks for each sign-in. The helper lives
// next to the Client executable (PLANK.app/Contents/MacOS/plank-passkey).
//
// Every call blocks until the helper exits; call them from a worker thread,
// never the GUI thread (assert waits for the user's Touch ID).
class PlankPasskeyHelper
{
public:
    // plank-passkey exit codes.
    enum ExitCode {
        Ok = 0,
        Failure = 1,
        InvalidInput = 2,
        NoMatchingKey = 3,
        NotConfirmed = 4,
    };

    struct Result {
        bool ran = false;       // started and exited normally (no crash/timeout)
        int exitCode = -1;
        QByteArray output;      // stdout (JSON)
        bool ok() const { return ran && exitCode == Ok; }
    };

    struct LocalKey {
        QString rpId;
        QString username;
        QString credentialId;
        QString mapping;
        QString created;
    };

    struct CreatedKey {
        QString credentialId;
        QString publicKey;
        QString mapping;   // "passkey:<credential id>,<SPKI>" for the admin; not secret
    };

    static constexpr int QuickTimeoutMs = 15000;
    // Device-key calls sit in front of broker requests (section 14.1).
    static constexpr int DeviceKeyTimeoutMs = 10000;
    // Below the broker's 120 s conversation expiry.
    static constexpr int AssertTimeoutMs = 110000;
    static constexpr int MaximumOutputBytes = 256 * 1024;

    explicit PlankPasskeyHelper(QString program = bundledProgram());

    // <application dir>/plank-passkey on macOS, empty elsewhere.
    static QString bundledProgram();
    bool available() const;
    const QString& program() const { return m_Program; }

    Result run(const QStringList& arguments, const QByteArray& input, int timeoutMs) const;

    bool list(const QString& rpId, QVector<LocalKey>& keys) const;
    bool create(const QString& rpId, const QString& username, CreatedKey& key) const;
    // Removes every local key of this user for this relying party.
    Result remove(const QString& rpId, const QString& username) const;

    // Signs the broker's passkey challenge (Touch ID sheet). The result is
    // validated against the request before it is returned.
    PlankBrokerClient::PasskeyAssertResult assertion(const PlankBroker::PasskeyRequest& request,
                                                     const QString& username,
                                                     PlankBroker::PasskeyAssertion& assertion) const;

    // Section 14.1 device key (no Touch ID; signed silently). brokerHost must
    // be a plain lower-case DNS host name (no IP literal), else nothing runs.
    // devicePublicKey creates the key on first use; deviceSign never creates
    // one. Both return an empty string on any failure (the Client then runs
    // unbound / sends no proof).
    QString devicePublicKey(const QString& brokerHost) const;
    QString deviceSign(const QString& brokerHost, const QByteArray& message) const;

    static QString parseDevicePublicKey(const QByteArray& output);
    static QString parseDeviceSignature(const QByteArray& output);

    static bool parseList(const QByteArray& output, QVector<LocalKey>& keys);
    static bool parseCreated(const QByteArray& output, CreatedKey& key);
    static PlankBrokerClient::PasskeyAssertResult classify(const Result& result);

private:
    QString m_Program;
};
