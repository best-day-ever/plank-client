#pragma once

// Per-workstation stream quality for remote-access (broker) hosts: capture
// source, encoding profile and a startup encoder target per route. Remote
// hosts have no LAN bookmark, so the choice lives in the Client's local
// settings next to the display setup (remote-hosts/<host id>), with a
// global "remote access defaults" layer (remote-defaults/) underneath and a
// built-in default at the bottom. Header-only so the broker test suite can
// exercise it without the GUI.
//
// Nothing here substitutes a profile silently: resolve() only picks which
// layer applies, and problemFor() reports a choice the workstation cannot
// use so the caller can ask the user instead of launching something else.

#include "settings/streamingpreferences.h"

#include <QCoreApplication>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVector>

namespace RemoteStreamSetup {

// Same values as NvOutputTopology::hostPlatform().
enum Platform { UnknownPlatform = 0, LinuxPlatform = 1, MacPlatform = 2 };

// The broker decides the route (bde-linux docs/plank-broker.md section 12);
// the office network is its direct route, the internet its relay.
enum Route { OfficeNetwork = 0, Internet = 1 };

// Unset: nothing saved for this workstation (a matching LAN bookmark may
// seed it). FollowDefaults: the user chose the remote access defaults.
// Custom: the user's own choice for this workstation.
enum Mode { Unset = 0, FollowDefaults = 1, Custom = 2 };

// Where the effective settings came from.
enum Source { FromHost = 0, FromBookmark = 1, FromDefaults = 2, FromBuiltIn = 3 };

// Mirrors NvOutputTopology::NvfbcHevc10NvencFeature (kept local so this
// header does not need the topology parser).
static constexpr int NvfbcHevc10NvencFeature = 0x2000;

struct Setup
{
    Mode mode = Unset;
    int captureSource = StreamingPreferences::PLANK_CAPTURE_NVFBC_8BIT;
    int videoProfile = StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_10BIT_444;
    QVector<int> officeBitratesKbps = StreamingPreferences::plankDefaultProfileBitrates();
    QVector<int> internetBitratesKbps = StreamingPreferences::plankDefaultProfileBitrates();
};

// What the workstation told us about itself on the last connect.
struct Capabilities
{
    bool known = false;
    int platform = UnknownPlatform;
    int featureFlags = 0;
    // Host's PlankEncodingModes; empty when it does not advertise them.
    QStringList encodingModes;
};

// A LAN bookmark that may seed a new workstation's first proposal.
struct BookmarkCandidate
{
    QString name;
    QString address;
    int captureSource = 0;
    int videoProfile = 0;
    QVector<int> bitratesKbps;
};

struct Resolution
{
    Setup setup;
    Source source = FromBuiltIn;
};

inline QString tr(const char* text)
{
    return QCoreApplication::translate("RemoteStreamSetup", text);
}

inline bool bitratesValid(const QVector<int>& bitrates)
{
    if (bitrates.size() != StreamingPreferences::PLANK_PROFILE_COUNT) return false;
    for (const int value : bitrates) {
        if (value != StreamingPreferences::clampPlankBitrate(value)) return false;
    }
    return true;
}

// Capture, profile and both bitrate lists are usable (not whether the
// workstation offers them; see problemFor()).
inline bool isValid(const Setup& setup)
{
    return StreamingPreferences::isPlankProfileValidForCaptureSource(setup.videoProfile, setup.captureSource) &&
           bitratesValid(setup.officeBitratesKbps) && bitratesValid(setup.internetBitratesKbps);
}

inline Setup builtInDefaults(int platform)
{
    Setup setup;
    setup.mode = FollowDefaults;
    if (platform == MacPlatform) {
        setup.captureSource = StreamingPreferences::PLANK_CAPTURE_SCREENCAPTUREKIT;
        setup.videoProfile = StreamingPreferences::PLANK_PROFILE_APPLE_HEVC_10BIT_420;
    } else {
        // The only Linux profile with a qualified hardware decode on Apple
        // Silicon clients; 50 Mbps (the HEVC default) on both routes.
        setup.captureSource = StreamingPreferences::PLANK_CAPTURE_NVFBC_8BIT;
        setup.videoProfile = StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_10BIT_444;
    }
    return setup;
}

inline bool fitsPlatform(const Setup& setup, int platform)
{
    if (platform == UnknownPlatform) return true;
    return (setup.captureSource == StreamingPreferences::PLANK_CAPTURE_SCREENCAPTUREKIT) ==
           (platform == MacPlatform);
}

inline int bitrateFor(const Setup& setup, Route route)
{
    return StreamingPreferences::plankBitrateForProfile(
                route == Internet ? setup.internetBitratesKbps : setup.officeBitratesKbps, setup.videoProfile);
}

inline const QVector<int>& bitratesFor(const Setup& setup, Route route)
{
    return route == Internet ? setup.internetBitratesKbps : setup.officeBitratesKbps;
}

// Bookmark lists are append-only (a short list gains defaults); anything
// unreadable is rejected rather than repaired.
inline bool readBitrates(const QVariant& value, QVector<int>& bitrates)
{
    if (!value.isValid()) return false;
    return StreamingPreferences::plankProfileBitratesFromVariantList(value.toList(), bitrates);
}

inline QString hostGroup(const QString& hostId)
{
    return QStringLiteral("remote-hosts/") + hostId.toLower();
}

inline QString defaultsGroup()
{
    return QStringLiteral("remote-defaults");
}

namespace detail {

inline bool readStream(QSettings& settings, Setup& setup)
{
    bool profileOk = false;
    bool captureOk = false;
    setup.videoProfile = settings.value(QStringLiteral("video-profile")).toInt(&profileOk);
    setup.captureSource = settings.value(QStringLiteral("capture-source")).toInt(&captureOk);
    return profileOk && captureOk &&
           readBitrates(settings.value(QStringLiteral("bitrates-office-kbps")), setup.officeBitratesKbps) &&
           readBitrates(settings.value(QStringLiteral("bitrates-internet-kbps")), setup.internetBitratesKbps) &&
           isValid(setup);
}

inline void writeStream(QSettings& settings, const Setup& setup)
{
    settings.setValue(QStringLiteral("video-profile"), setup.videoProfile);
    settings.setValue(QStringLiteral("capture-source"), setup.captureSource);
    settings.setValue(QStringLiteral("bitrates-office-kbps"),
                      StreamingPreferences::plankProfileBitratesToVariantList(setup.officeBitratesKbps));
    settings.setValue(QStringLiteral("bitrates-internet-kbps"),
                      StreamingPreferences::plankProfileBitratesToVariantList(setup.internetBitratesKbps));
}

inline void removeStream(QSettings& settings)
{
    for (const char* key : {"video-profile", "capture-source", "bitrates-office-kbps", "bitrates-internet-kbps"}) {
        settings.remove(QLatin1String(key));
    }
}

}

// Per-workstation keys live in a "stream" subgroup of the display setup's
// group, so both are forgotten together.
inline Setup loadHost(QSettings& settings, const QString& hostId)
{
    Setup setup;
    settings.beginGroup(hostGroup(hostId) + QStringLiteral("/stream"));
    const QString mode = settings.value(QStringLiteral("mode")).toString();
    if (mode == QLatin1String("defaults")) {
        setup.mode = FollowDefaults;
    } else if (mode == QLatin1String("custom")) {
        // Invalid (older build, hand edit, unknown profile): treated as unset,
        // never repaired into something else at connect time.
        setup.mode = detail::readStream(settings, setup) ? Custom : Unset;
    }
    settings.endGroup();
    if (setup.mode != Custom) {
        const Setup fresh;
        setup.captureSource = fresh.captureSource;
        setup.videoProfile = fresh.videoProfile;
        setup.officeBitratesKbps = fresh.officeBitratesKbps;
        setup.internetBitratesKbps = fresh.internetBitratesKbps;
    }
    return setup;
}

inline bool saveHost(QSettings& settings, const QString& hostId, const Setup& setup)
{
    if (setup.mode == Custom && !isValid(setup)) return false;
    settings.beginGroup(hostGroup(hostId) + QStringLiteral("/stream"));
    detail::removeStream(settings);
    switch (setup.mode) {
    case Custom:
        settings.setValue(QStringLiteral("mode"), QStringLiteral("custom"));
        detail::writeStream(settings, setup);
        break;
    case FollowDefaults:
        settings.setValue(QStringLiteral("mode"), QStringLiteral("defaults"));
        break;
    case Unset:
        settings.remove(QStringLiteral("mode"));
        break;
    }
    settings.endGroup();
    return true;
}

// Global remote access defaults; mode is Custom when the user saved valid
// defaults, Unset otherwise (the built-in default then applies).
inline Setup loadDefaults(QSettings& settings)
{
    Setup setup;
    settings.beginGroup(defaultsGroup());
    setup.mode = detail::readStream(settings, setup) ? Custom : Unset;
    settings.endGroup();
    if (setup.mode != Custom) {
        return Setup();
    }
    return setup;
}

inline bool saveDefaults(QSettings& settings, const Setup& setup)
{
    if (!isValid(setup)) return false;
    settings.beginGroup(defaultsGroup());
    detail::writeStream(settings, setup);
    settings.endGroup();
    return true;
}

inline void clearDefaults(QSettings& settings)
{
    settings.remove(defaultsGroup());
}

inline Capabilities loadCapabilities(QSettings& settings, const QString& hostId)
{
    Capabilities caps;
    settings.beginGroup(hostGroup(hostId) + QStringLiteral("/host"));
    bool ok = false;
    const int platform = settings.value(QStringLiteral("platform")).toInt(&ok);
    if (ok && (platform == LinuxPlatform || platform == MacPlatform)) {
        caps.known = true;
        caps.platform = platform;
        caps.featureFlags = settings.value(QStringLiteral("feature-flags")).toInt();
        const QString modes = settings.value(QStringLiteral("encoding-modes")).toString();
        caps.encodingModes = modes.split(QLatin1Char(','), Qt::SkipEmptyParts);
    }
    settings.endGroup();
    return caps;
}

inline void saveCapabilities(QSettings& settings, const QString& hostId, const Capabilities& caps)
{
    if (!caps.known) return;
    settings.beginGroup(hostGroup(hostId) + QStringLiteral("/host"));
    settings.setValue(QStringLiteral("platform"), caps.platform);
    settings.setValue(QStringLiteral("feature-flags"), caps.featureFlags);
    settings.setValue(QStringLiteral("encoding-modes"), caps.encodingModes.join(QLatin1Char(',')));
    settings.endGroup();
}

inline QStringList parseEncodingModes(const QString& advertised)
{
    QStringList modes;
    for (const QString& mode : advertised.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QString trimmed = mode.trimmed();
        if (!trimmed.isEmpty() && !modes.contains(trimmed)) modes.append(trimmed);
    }
    return modes;
}

// Seeds a new workstation from a LAN bookmark only on an exact match: the
// bookmark's address is the workstation's host id or name, or its name is
// the workstation's name. Several different matches seed nothing.
inline bool seedFromBookmarks(const QVector<BookmarkCandidate>& bookmarks, const QString& hostId,
                              const QString& hostName, Setup& seed)
{
    bool found = false;
    Setup candidate;
    for (const BookmarkCandidate& bookmark : bookmarks) {
        const bool matches =
                (!hostId.isEmpty() && bookmark.address.compare(hostId, Qt::CaseInsensitive) == 0) ||
                (!hostName.isEmpty() && (bookmark.address.compare(hostName, Qt::CaseInsensitive) == 0 ||
                                         bookmark.name.compare(hostName, Qt::CaseInsensitive) == 0));
        if (!matches) continue;
        Setup next;
        next.mode = Custom;
        next.captureSource = bookmark.captureSource;
        next.videoProfile = bookmark.videoProfile;
        QVector<int> bitrates = bookmark.bitratesKbps;
        if (!bitratesValid(bitrates)) bitrates = StreamingPreferences::plankDefaultProfileBitrates();
        // A LAN bookmark's targets were chosen for the office network.
        next.officeBitratesKbps = bitrates;
        next.internetBitratesKbps = StreamingPreferences::plankDefaultProfileBitrates();
        if (!isValid(next)) continue;
        if (found && (next.captureSource != candidate.captureSource ||
                      next.videoProfile != candidate.videoProfile ||
                      next.officeBitratesKbps != candidate.officeBitratesKbps)) {
            return false;
        }
        candidate = next;
        found = true;
    }
    if (found) seed = candidate;
    return found;
}

// Layering: this workstation's own choice, then (only while nothing is saved
// for it) an exactly matching LAN bookmark, then the remote access defaults,
// then the built-in default for the platform. A layer that does not fit a
// known platform (a Linux default for a Mac workstation) is skipped; the
// workstation's own choice never is (problemFor() reports it instead).
inline Resolution resolve(const Setup& host, const Setup* seed, const Setup& defaults, int platform)
{
    Resolution result;
    if (host.mode == Custom) {
        result.setup = host;
        result.source = FromHost;
        return result;
    }
    if (host.mode == Unset && seed != nullptr && isValid(*seed) && fitsPlatform(*seed, platform)) {
        result.setup = *seed;
        result.source = FromBookmark;
        return result;
    }
    if (defaults.mode == Custom && isValid(defaults) && fitsPlatform(defaults, platform)) {
        result.setup = defaults;
        result.source = FromDefaults;
        return result;
    }
    result.setup = builtInDefaults(platform);
    result.source = FromBuiltIn;
    return result;
}

inline QString profileName(int profile)
{
    switch (profile) {
    case StreamingPreferences::PLANK_PROFILE_H264_8BIT_422: return tr("H.264 8-bit 4:2:2");
    case StreamingPreferences::PLANK_PROFILE_H264_8BIT_444: return tr("H.264 8-bit 4:4:4 (identity GBR)");
    case StreamingPreferences::PLANK_PROFILE_H264_10BIT_422: return tr("H.264 10-bit 4:2:2");
    case StreamingPreferences::PLANK_PROFILE_H264_10BIT_444: return tr("H.264 10-bit 4:4:4 (identity GBR)");
    case StreamingPreferences::PLANK_PROFILE_NVENC_H264_8BIT_444: return tr("H.264 8-bit 4:4:4 (identity GBR) — NVENC");
    case StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_8BIT_444: return tr("H.265 8-bit 4:4:4 (identity GBR) — NVENC");
    case StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_10BIT_444: return tr("H.265 10-bit 4:4:4 (identity GBR) — NVENC");
    case StreamingPreferences::PLANK_PROFILE_APPLE_HEVC_10BIT_420: return tr("HEVC 10-bit 4:2:0 — Apple VideoToolbox");
    case StreamingPreferences::PLANK_PROFILE_APPLE_HEVC_10BIT_444: return tr("HEVC 10-bit 4:4:4 — Apple VideoToolbox");
    default: return tr("an unknown encoding profile");
    }
}

// Why this workstation cannot stream with this capture source and profile
// (user-facing), or an empty string when it can as far as the Client knows.
// Unknown capabilities only check the capture/profile pairing.
inline QString problemFor(int captureSource, int profile, const Capabilities& caps)
{
    if (!StreamingPreferences::isPlankProfileValidForCaptureSource(profile, captureSource)) {
        return tr("The capture source and encoding profile do not fit together. Choose another encoding.");
    }
    if (!caps.known) return QString();
    const bool sck = captureSource == StreamingPreferences::PLANK_CAPTURE_SCREENCAPTUREKIT;
    if (caps.platform == MacPlatform && !sck) {
        return tr("This workstation is a Mac. Choose ScreenCaptureKit capture and an Apple encoding.");
    }
    if (caps.platform == LinuxPlatform && sck) {
        return tr("This workstation is not a Mac and can't use ScreenCaptureKit capture. Choose another capture source.");
    }
    const QString mode = StreamingPreferences::plankEncodingMode(profile);
    if (!caps.encodingModes.isEmpty() && !caps.encodingModes.contains(mode)) {
        return tr("This workstation can't use %1. Choose another encoding.").arg(profileName(profile));
    }
    if (caps.platform == LinuxPlatform &&
            captureSource == StreamingPreferences::PLANK_CAPTURE_NVFBC_8BIT &&
            profile == StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_10BIT_444 &&
            (caps.featureFlags & NvfbcHevc10NvencFeature) == 0) {
        return tr("This workstation can't use %1 with NvFBC capture. Choose another encoding or capture source.")
                .arg(profileName(profile));
    }
    return QString();
}

inline QString problemFor(const Setup& setup, const Capabilities& caps)
{
    return problemFor(setup.captureSource, setup.videoProfile, caps);
}

}
