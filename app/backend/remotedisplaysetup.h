#pragma once

// Per-workstation display setup for remote-access (broker) hosts. Remote hosts
// have no LAN bookmark, so the user picks the layout on first connect and it
// is kept in the Client's local settings under remote-hosts/<host id>.
// Header-only so the broker test suite can exercise it without the GUI.

#include "outputtopology.h"

#include <QSettings>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

namespace RemoteDisplaySetup {

// Host layout choices, in the order the dialog shows them.
enum LayoutChoice { MatchClient = 0, Physical = 1, SingleVirtual = 2, DualVirtual = 3 };
enum ScalingChoice { Native = 0, ScaledSpan = 1 };

struct Setup
{
    bool configured = false;
    QString hostLayout;
    QString virtualMode1;
    QString virtualMode2;
    QString scalingMode;
};

inline QString layoutForChoice(int choice)
{
    switch (choice) {
    case MatchClient: return QString::fromLatin1(NvOutputTopology::MatchClientHostLayout);
    case Physical: return QString::fromLatin1(NvOutputTopology::PhysicalHostLayout);
    case SingleVirtual: return QString::fromLatin1(NvOutputTopology::SingleHostLayout);
    case DualVirtual: return QString::fromLatin1(NvOutputTopology::DualHorizontalHostLayout);
    default: return QString();
    }
}

inline int choiceForLayout(const QString& layout)
{
    for (int choice = MatchClient; choice <= DualVirtual; ++choice) {
        if (layoutForChoice(choice) == layout) return choice;
    }
    return -1;
}

inline QString scalingForChoice(int choice)
{
    return QString::fromLatin1(choice == Native ? NvOutputTopology::NativeScalingMode :
                                                  NvOutputTopology::ScaledSpanMode);
}

inline int choiceForScaling(const QString& scaling)
{
    return scaling == QLatin1String(NvOutputTopology::NativeScalingMode) ? Native : ScaledSpan;
}

inline bool isQualifiedMode(const QString& mode)
{
    return NvOutputTopology::qualifiedVirtualModes().contains(mode);
}

inline bool isValid(const Setup& setup)
{
    const int choice = choiceForLayout(setup.hostLayout);
    if (choice < 0) return false;
    if (setup.scalingMode != QLatin1String(NvOutputTopology::NativeScalingMode) &&
            setup.scalingMode != QLatin1String(NvOutputTopology::ScaledSpanMode)) {
        return false;
    }
    // Virtual modes are always stored (a Mac host uses mode 1 as its desktop).
    return isQualifiedMode(setup.virtualMode1) && isQualifiedMode(setup.virtualMode2);
}

inline QString settingsGroup(const QString& hostId)
{
    return QStringLiteral("remote-hosts/") + hostId.toLower();
}

inline Setup load(QSettings& settings, const QString& hostId)
{
    Setup setup;
    settings.beginGroup(settingsGroup(hostId));
    setup.hostLayout = settings.value(QStringLiteral("host-layout")).toString();
    setup.virtualMode1 = settings.value(QStringLiteral("virtual-mode-1")).toString();
    setup.virtualMode2 = settings.value(QStringLiteral("virtual-mode-2")).toString();
    setup.scalingMode = settings.value(QStringLiteral("scaling-mode")).toString();
    settings.endGroup();
    // Anything unreadable (older build, hand edit) is simply asked again.
    setup.configured = isValid(setup);
    return setup;
}

inline bool save(QSettings& settings, const QString& hostId, const Setup& setup)
{
    if (!isValid(setup)) return false;
    settings.beginGroup(settingsGroup(hostId));
    settings.setValue(QStringLiteral("host-layout"), setup.hostLayout);
    settings.setValue(QStringLiteral("virtual-mode-1"), setup.virtualMode1);
    settings.setValue(QStringLiteral("virtual-mode-2"), setup.virtualMode2);
    settings.setValue(QStringLiteral("scaling-mode"), setup.scalingMode);
    settings.endGroup();
    return true;
}

// Largest qualified virtual mode that fits inside the client panel, so the
// stream is never upscaled; the smallest standard mode if none fits.
inline QString suggestedMode(const QSize& clientNativeSize)
{
    QString best;
    qint64 bestArea = -1;
    for (const QString& mode : NvOutputTopology::qualifiedVirtualModes()) {
        const QStringList parts = mode.split(QLatin1Char('x'));
        if (parts.size() != 2) continue;
        const int width = parts[0].toInt();
        const int height = parts[1].toInt();
        // Ultra-tall halves (1024/1280/2560x2160) only make sense as a pair.
        if (height > width) continue;
        if (width > clientNativeSize.width() || height > clientNativeSize.height()) continue;
        const qint64 area = qint64(width) * height;
        if (area > bestArea) {
            bestArea = area;
            best = mode;
        }
    }
    return best.isEmpty() ? QStringLiteral("1920x1080") : best;
}

// Whether "Match my displays" can work for these client displays; the reason
// is user-facing when it cannot.
inline bool canMatchClient(const QVector<NvClientDisplay>& displays, QString* reason = nullptr)
{
    QString layout;
    QStringList modes;
    QString error;
    const bool ok = NvOutputTopology::resolveClientDisplayLayout(displays, layout, modes, &error);
    if (!ok && reason != nullptr) *reason = error;
    return ok;
}

// First-connect proposal: match the client when its displays qualify,
// otherwise one virtual display that fits the main panel, scaled to fit.
inline Setup proposal(const QVector<NvClientDisplay>& displays)
{
    Setup setup;
    const QSize primary = displays.isEmpty() ? QSize(1920, 1080) : displays.first().nativeSize;
    const QString mode = suggestedMode(primary);
    setup.hostLayout = layoutForChoice(canMatchClient(displays) ? MatchClient : SingleVirtual);
    setup.virtualMode1 = mode;
    setup.virtualMode2 = mode;
    setup.scalingMode = QString::fromLatin1(NvOutputTopology::ScaledSpanMode);
    return setup;
}

}
