#pragma once

// Whether the Client opens with the first sign-in wizard. Only a truly new
// installation does: anything that shows this Mac was used before - the
// "completed" flag (set by the wizard and by every broker sign-in,
// RemoteBroker::finishSignIn), a remembered remote session, a local
// Touch ID key, a saved workstation bookmark or a remote display setup -
// means the person already knows the way, and they get the normal sign-in.
// Settings written by older Clients (same settings domain) therefore keep
// existing users out of the wizard without any migration.

#include <QDir>
#include <QDirIterator>
#include <QSettings>
#include <QString>
#include <QStringList>

namespace OnboardingState
{

inline QString completedKey() { return QStringLiteral("onboarding/completed"); }

struct Inputs {
    bool completed = false;          // onboarding/completed
    bool brokerConfigured = true;    // the wizard needs a reachable, pinned broker
    bool rememberedSession = false;  // Keychain broker session
    bool localPasskeys = false;      // plank-passkey keys on this Mac
    bool bookmarks = false;          // hosts/size > 0 (LAN workstations)
    bool remoteHosts = false;        // remote-hosts/* display setups
};

// The first input (in this order) that rules the wizard out.
enum class Decision {
    Show,
    Completed,
    NotConfigured,
    RememberedSession,
    LocalPasskeys,
    Bookmarks,
    RemoteHosts,
};

inline Decision decide(const Inputs& inputs)
{
    if (inputs.completed) return Decision::Completed;
    if (!inputs.brokerConfigured) return Decision::NotConfigured;
    if (inputs.rememberedSession) return Decision::RememberedSession;
    if (inputs.localPasskeys) return Decision::LocalPasskeys;
    if (inputs.bookmarks) return Decision::Bookmarks;
    if (inputs.remoteHosts) return Decision::RemoteHosts;
    return Decision::Show;
}

inline bool shouldShow(const Inputs& inputs) { return decide(inputs) == Decision::Show; }

// The settings-backed inputs (flag, bookmarks, remote display setups).
inline void readSettings(QSettings& settings, Inputs& inputs)
{
    inputs.completed = settings.value(completedKey(), false).toBool();
    // ComputerManager keeps bookmarks in the "hosts" array (and a backup
    // copy while a save is in flight).
    const int hosts = settings.beginReadArray(QStringLiteral("hosts"));
    settings.endArray();
    const int backup = settings.beginReadArray(QStringLiteral("hostsbackup"));
    settings.endArray();
    inputs.bookmarks = hosts > 0 || backup > 0;
    settings.beginGroup(QStringLiteral("remote-hosts"));
    inputs.remoteHosts = !settings.childGroups().isEmpty();
    settings.endGroup();
}

inline void markCompleted(QSettings& settings)
{
    settings.setValue(completedKey(), true);
    settings.sync();
}

// Every successful broker sign-in (password + code, Touch ID, or the
// wizard's own) calls this. The Keychain session is not lasting proof of
// earlier use: sign-out and an expired session (401) clear it.
inline void recordBrokerSignIn(QSettings& settings) { markCompleted(settings); }

// True when the plank-passkey store (<root>/<rp_id>/<user>/<id>.json)
// holds at least one key, for any relying party.
inline bool hasLocalPasskeys(const QString& storeRoot)
{
    if (storeRoot.isEmpty() || !QDir(storeRoot).exists()) return false;
    QDirIterator it(storeRoot, {QStringLiteral("*.json")}, QDir::Files, QDirIterator::Subdirectories);
    return it.hasNext();
}

}
