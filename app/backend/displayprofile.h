#pragma once

// The user's display setup intent for one monitor set (identified by
// ClientDisplayProbe::fingerprint): which monitors the workstation uses, at
// what size, which one is primary, and how the stream is presented. The
// planner (displayplanner.h) turns a profile plus the live probe into a
// layout; the profile never stores pixel positions unless the user placed
// the displays by hand (manual).
//
// Storage (the Client's QSettings):
//   display-profiles/sets/<fp>/{profile,label,saved}   global: every
//        workstation has the same dongle, so one layout serves them all
//   remote-hosts/<id>/display/mode                     follow | custom | legacy
//   remote-hosts/<id>/display/sets/<fp>/profile        "custom for this workstation"
//   display-profiles/{ask-on-change,auto-accept}        Settings > Displays
// A workstation in legacy mode keeps using the pre-profile keys
// (remote-hosts/<id>/host-layout, virtual-mode-1/2, scaling-mode), which stay
// authoritative; migration never deletes them.
//
// Header-only so the broker and planner test suites exercise it without the GUI.

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPoint>
#include <QSettings>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>

namespace DisplayProfile {

static constexpr int Version = 1;

enum class SizeMode { Exact, LooksLike, Preset, Custom };

struct MonitorChoice
{
    QString key;
    bool on = true;
    SizeMode size = SizeMode::Exact;
    QSize fixedSize;          // Preset and Custom
    bool hasPosition = false; // manual placement only
    QPoint position;
    QString backing = QStringLiteral("auto"); // auto | physical | virtual
};

struct Profile
{
    QVector<MonitorChoice> monitors;
    QString primary;                                  // monitor key; empty: the menu-bar display
    QString presentation = QStringLiteral("windows"); // windows | single
    QString scaling = QStringLiteral("native");       // native | fit
    bool manual = false;

    const MonitorChoice* find(const QString& key) const
    {
        for (const MonitorChoice& monitor : monitors) {
            if (monitor.key == key) return &monitor;
        }
        return nullptr;
    }

    MonitorChoice& choice(const QString& key)
    {
        for (MonitorChoice& monitor : monitors) {
            if (monitor.key == key) return monitor;
        }
        MonitorChoice monitor;
        monitor.key = key;
        monitors.append(monitor);
        return monitors.last();
    }
};

inline QString sizeText(const MonitorChoice& monitor)
{
    switch (monitor.size) {
    case SizeMode::LooksLike: return QStringLiteral("looks-like");
    case SizeMode::Preset:
        return QStringLiteral("preset:%1x%2").arg(monitor.fixedSize.width()).arg(monitor.fixedSize.height());
    case SizeMode::Custom:
        return QStringLiteral("custom:%1x%2").arg(monitor.fixedSize.width()).arg(monitor.fixedSize.height());
    case SizeMode::Exact: default: return QStringLiteral("exact");
    }
}

inline bool sizeFromText(const QString& text, MonitorChoice& monitor)
{
    if (text == QLatin1String("exact")) {
        monitor.size = SizeMode::Exact;
        monitor.fixedSize = QSize();
        return true;
    }
    if (text == QLatin1String("looks-like")) {
        monitor.size = SizeMode::LooksLike;
        monitor.fixedSize = QSize();
        return true;
    }
    const bool preset = text.startsWith(QLatin1String("preset:"));
    if (!preset && !text.startsWith(QLatin1String("custom:"))) return false;
    const QString size = text.mid(7);
    const int separator = size.indexOf(QLatin1Char('x'));
    bool widthOk = false;
    bool heightOk = false;
    const int width = size.left(separator).toInt(&widthOk);
    const int height = size.mid(separator + 1).toInt(&heightOk);
    // Even sizes within what any host could drive.
    if (separator <= 0 || !widthOk || !heightOk || width < 2 || height < 2 || width > 16384 || height > 16384 ||
            (width & 1) || (height & 1) || QStringLiteral("%1x%2").arg(width).arg(height) != size) {
        return false;
    }
    monitor.size = preset ? SizeMode::Preset : SizeMode::Custom;
    monitor.fixedSize = QSize(width, height);
    return true;
}

inline QJsonObject toJson(const Profile& profile)
{
    QJsonArray monitors;
    for (const MonitorChoice& monitor : profile.monitors) {
        QJsonObject entry {{QStringLiteral("key"), monitor.key}, {QStringLiteral("on"), monitor.on},
                           {QStringLiteral("size"), sizeText(monitor)},
                           {QStringLiteral("backing"), monitor.backing}};
        if (monitor.hasPosition) {
            entry.insert(QStringLiteral("pos"), QJsonObject {{QStringLiteral("x"), monitor.position.x()},
                                                             {QStringLiteral("y"), monitor.position.y()}});
        }
        monitors.append(entry);
    }
    return {{QStringLiteral("v"), Version}, {QStringLiteral("monitors"), monitors},
            {QStringLiteral("primary"), profile.primary}, {QStringLiteral("presentation"), profile.presentation},
            {QStringLiteral("scaling"), profile.scaling}, {QStringLiteral("manual"), profile.manual}};
}

// Anything unreadable (another version, a hand edit) is rejected as a whole:
// the caller then treats the monitor set as new and asks again.
inline bool fromJson(const QJsonObject& object, Profile& result)
{
    if (object.value(QStringLiteral("v")).toInt(-1) != Version || !object.value(QStringLiteral("monitors")).isArray()) {
        return false;
    }
    Profile profile;
    profile.primary = object.value(QStringLiteral("primary")).toString();
    profile.presentation = object.value(QStringLiteral("presentation")).toString(QStringLiteral("windows"));
    profile.scaling = object.value(QStringLiteral("scaling")).toString(QStringLiteral("native"));
    profile.manual = object.value(QStringLiteral("manual")).toBool(false);
    if ((profile.presentation != QLatin1String("windows") && profile.presentation != QLatin1String("single")) ||
            (profile.scaling != QLatin1String("native") && profile.scaling != QLatin1String("fit"))) {
        return false;
    }
    for (const QJsonValue& value : object.value(QStringLiteral("monitors")).toArray()) {
        const QJsonObject entry = value.toObject();
        MonitorChoice monitor;
        monitor.key = entry.value(QStringLiteral("key")).toString();
        monitor.on = entry.value(QStringLiteral("on")).toBool(true);
        monitor.backing = entry.value(QStringLiteral("backing")).toString(QStringLiteral("auto"));
        if (monitor.key.isEmpty() || monitor.key.size() > 256 || profile.find(monitor.key) != nullptr ||
                !sizeFromText(entry.value(QStringLiteral("size")).toString(QStringLiteral("exact")), monitor) ||
                (monitor.backing != QLatin1String("auto") && monitor.backing != QLatin1String("physical") &&
                 monitor.backing != QLatin1String("virtual"))) {
            return false;
        }
        const QJsonValue position = entry.value(QStringLiteral("pos"));
        if (position.isObject()) {
            const QJsonObject pos = position.toObject();
            if (!pos.value(QStringLiteral("x")).isDouble() || !pos.value(QStringLiteral("y")).isDouble()) return false;
            monitor.hasPosition = true;
            monitor.position = QPoint(pos.value(QStringLiteral("x")).toInt(), pos.value(QStringLiteral("y")).toInt());
        } else if (!position.isUndefined()) {
            return false;
        }
        profile.monitors.append(monitor);
    }
    result = profile;
    return true;
}

inline QString encode(const Profile& profile)
{
    return QString::fromUtf8(QJsonDocument(toJson(profile)).toJson(QJsonDocument::Compact));
}

inline bool decode(const QString& text, Profile& profile)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(text.toUtf8(), &error);
    return error.error == QJsonParseError::NoError && document.isObject() && fromJson(document.object(), profile);
}

// ---- Storage ---------------------------------------------------------------

enum class HostMode { Follow, Custom, Legacy };

inline bool validFingerprint(const QString& fingerprint)
{
    if (fingerprint.isEmpty() || fingerprint.size() > 64) return false;
    for (const QChar c : fingerprint) {
        if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9')) || (c >= QLatin1Char('a') && c <= QLatin1Char('f')))) {
            return false;
        }
    }
    return true;
}

inline QString setsGroup()
{
    return QStringLiteral("display-profiles/sets");
}

inline QString hostGroup(const QString& hostId)
{
    return QStringLiteral("remote-hosts/") + hostId.toLower() + QStringLiteral("/display");
}

inline bool loadGlobal(QSettings& settings, const QString& fingerprint, Profile& profile)
{
    if (!validFingerprint(fingerprint)) return false;
    return decode(settings.value(setsGroup() + QLatin1Char('/') + fingerprint + QStringLiteral("/profile")).toString(),
                  profile);
}

inline void saveGlobal(QSettings& settings, const QString& fingerprint, const QString& label, const Profile& profile)
{
    if (!validFingerprint(fingerprint)) return;
    settings.beginGroup(setsGroup() + QLatin1Char('/') + fingerprint);
    settings.setValue(QStringLiteral("profile"), encode(profile));
    settings.setValue(QStringLiteral("label"), label);
    settings.setValue(QStringLiteral("saved"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    settings.endGroup();
}

inline void forgetGlobal(QSettings& settings, const QString& fingerprint)
{
    if (!validFingerprint(fingerprint)) return;
    settings.remove(setsGroup() + QLatin1Char('/') + fingerprint);
}

struct SavedSet
{
    QString fingerprint;
    QString label;
    QString saved; // ISO 8601 UTC
};

// Saved global layouts, most recent first.
inline QVector<SavedSet> savedSets(QSettings& settings)
{
    QVector<SavedSet> sets;
    settings.beginGroup(setsGroup());
    for (const QString& fingerprint : settings.childGroups()) {
        Profile profile;
        if (!validFingerprint(fingerprint) ||
                !decode(settings.value(fingerprint + QStringLiteral("/profile")).toString(), profile)) {
            continue;
        }
        sets.append({fingerprint, settings.value(fingerprint + QStringLiteral("/label")).toString(),
                     settings.value(fingerprint + QStringLiteral("/saved")).toString()});
    }
    settings.endGroup();
    std::sort(sets.begin(), sets.end(), [](const SavedSet& a, const SavedSet& b) {
        return a.saved != b.saved ? a.saved > b.saved : a.fingerprint < b.fingerprint;
    });
    return sets;
}

inline HostMode hostMode(QSettings& settings, const QString& hostId)
{
    const QString mode = settings.value(hostGroup(hostId) + QStringLiteral("/mode")).toString();
    if (mode == QLatin1String("custom")) return HostMode::Custom;
    if (mode == QLatin1String("legacy")) return HostMode::Legacy;
    return HostMode::Follow;
}

inline bool hostModeSaved(QSettings& settings, const QString& hostId)
{
    return settings.contains(hostGroup(hostId) + QStringLiteral("/mode"));
}

inline void setHostMode(QSettings& settings, const QString& hostId, HostMode mode)
{
    settings.setValue(hostGroup(hostId) + QStringLiteral("/mode"),
                      mode == HostMode::Custom ? QStringLiteral("custom") :
                      mode == HostMode::Legacy ? QStringLiteral("legacy") : QStringLiteral("follow"));
}

inline bool loadHost(QSettings& settings, const QString& hostId, const QString& fingerprint, Profile& profile)
{
    if (!validFingerprint(fingerprint)) return false;
    return decode(settings.value(hostGroup(hostId) + QStringLiteral("/sets/") + fingerprint +
                                 QStringLiteral("/profile")).toString(), profile);
}

inline void saveHost(QSettings& settings, const QString& hostId, const QString& fingerprint, const Profile& profile)
{
    if (!validFingerprint(fingerprint)) return;
    settings.setValue(hostGroup(hostId) + QStringLiteral("/sets/") + fingerprint + QStringLiteral("/profile"),
                      encode(profile));
}

inline void forgetHost(QSettings& settings, const QString& hostId, const QString& fingerprint)
{
    if (!validFingerprint(fingerprint)) return;
    settings.remove(hostGroup(hostId) + QStringLiteral("/sets/") + fingerprint);
}

// Settings > Displays.
inline bool askOnChange(QSettings& settings)
{
    return settings.value(QStringLiteral("display-profiles/ask-on-change"), true).toBool();
}

inline void setAskOnChange(QSettings& settings, bool ask)
{
    settings.setValue(QStringLiteral("display-profiles/ask-on-change"), ask);
}

inline bool autoAccept(QSettings& settings)
{
    return settings.value(QStringLiteral("display-profiles/auto-accept"), false).toBool();
}

inline void setAutoAccept(QSettings& settings, bool accept)
{
    settings.setValue(QStringLiteral("display-profiles/auto-accept"), accept);
}

inline bool chooseBannerPending(QSettings& settings)
{
    return settings.value(QStringLiteral("display-profiles/choose-banner"), false).toBool();
}

inline void clearChooseBanner(QSettings& settings)
{
    settings.setValue(QStringLiteral("display-profiles/choose-banner"), false);
}

// What a connect to this workstation uses on this monitor set.
struct Resolved
{
    enum Source { None, Global, HostCustom, Legacy };
    Source source = None;
    Profile profile;
};

// Resolution order: a workstation kept on its older layout (legacy) uses the
// pre-profile keys; one set to "custom" uses its own layout for this monitor
// set when there is one; otherwise the global layout for the monitor set;
// otherwise nothing (the monitor set is new: ask, or accept the proposal).
inline Resolved resolveForHost(QSettings& settings, const QString& hostId, const QString& fingerprint)
{
    Resolved resolved;
    if (!hostId.isEmpty()) {
        const HostMode mode = hostMode(settings, hostId);
        if (mode == HostMode::Legacy) {
            resolved.source = Resolved::Legacy;
            return resolved;
        }
        if (mode == HostMode::Custom && loadHost(settings, hostId, fingerprint, resolved.profile)) {
            resolved.source = Resolved::HostCustom;
            return resolved;
        }
    }
    if (loadGlobal(settings, fingerprint, resolved.profile)) {
        resolved.source = Resolved::Global;
        return resolved;
    }
    resolved.profile = Profile();
    return resolved;
}

// One-time, idempotent migration of the per-workstation layouts saved before
// display profiles. Old keys are never deleted and stay authoritative for
// workstations that keep a fixed layout:
//   host-layout=match-client                  -> display/mode=follow
//   host-layout=physical|single|dual-horizontal -> display/mode=legacy
// A Match client user's current monitor set gets the proposal saved silently
// (when it has no layout yet) and the "choose your screens" banner once.
// Returns the number of workstations migrated.
inline int migrate(QSettings& settings, const QString& fingerprint, const QString& label, const Profile& proposal)
{
    int migrated = 0;
    bool matchUsers = false;
    settings.beginGroup(QStringLiteral("remote-hosts"));
    const QStringList hosts = settings.childGroups();
    settings.endGroup();
    for (const QString& host : hosts) {
        if (hostModeSaved(settings, host)) continue;
        const QString layout = settings.value(QStringLiteral("remote-hosts/") + host +
                                              QStringLiteral("/host-layout")).toString();
        if (layout == QLatin1String("match-client")) {
            setHostMode(settings, host, HostMode::Follow);
            matchUsers = true;
            ++migrated;
        } else if (layout == QLatin1String("physical") || layout == QLatin1String("single") ||
                   layout == QLatin1String("dual-horizontal")) {
            setHostMode(settings, host, HostMode::Legacy);
            ++migrated;
        }
    }
    if (matchUsers && !settings.value(QStringLiteral("display-profiles/migrated")).toBool()) {
        Profile existing;
        if (!loadGlobal(settings, fingerprint, existing)) {
            saveGlobal(settings, fingerprint, label, proposal);
        }
        settings.setValue(QStringLiteral("display-profiles/choose-banner"), true);
    }
    settings.setValue(QStringLiteral("display-profiles/migrated"), true);
    return migrated;
}

}
