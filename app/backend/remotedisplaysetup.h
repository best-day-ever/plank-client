#pragma once

// Per-workstation display setup for remote-access (broker) hosts. Remote hosts
// have no LAN bookmark, so the user picks the layout on first connect and it
// is kept in the Client's local settings under remote-hosts/<host id>.
// Header-only so the broker test suite can exercise it without the GUI.

#include "outputtopology.h"

#include <QCoreApplication>
#include <QPoint>
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
    // Match client re-resolves its virtual modes from the client displays on
    // every connect, so stale stored modes (an older build stored whatever
    // the display dialog guessed) must not force the dialog again.
    if (setup.hostLayout == QLatin1String(NvOutputTopology::MatchClientHostLayout)) {
        if (!isQualifiedMode(setup.virtualMode1)) setup.virtualMode1 = QStringLiteral("1920x1080");
        if (!isQualifiedMode(setup.virtualMode2)) setup.virtualMode2 = setup.virtualMode1;
    }
    // Anything else unreadable (older build, hand edit) is simply asked again.
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

// The virtual modes to offer and match against for one workstation: what its
// feature flags from the last connect say it accepts, or every qualified mode
// while nothing is known yet (the Session re-resolves Match client with the
// flags of the connect itself and refuses a mode the host does not accept).
inline QStringList candidateModes(bool hostKnown, int hostFeatureFlags)
{
    return hostKnown ? NvOutputTopology::virtualModesForHost(hostFeatureFlags) :
                       NvOutputTopology::qualifiedVirtualModes();
}

// Best qualified virtual mode for one virtual display on this client: the
// same ranking Match client uses (exact, else the closest aspect ratio that
// fits without upscaling), restricted to landscape modes because the
// ultra-tall halves only make sense as a pair.
inline QString suggestedMode(const QSize& clientSize,
                             const QStringList& modes = NvOutputTopology::qualifiedVirtualModes())
{
    for (const QString& mode : NvOutputTopology::rankedVirtualModes(clientSize, modes)) {
        const QSize size = NvOutputTopology::virtualModeSize(mode);
        if (size.width() >= size.height()) return mode;
    }
    return QStringLiteral("1920x1080");
}

// Whether "Match my displays" can work for these client displays; the reason
// is user-facing when it cannot. Any single display or left-to-right pair
// can: odd sizes are matched to the closest supported mode.
inline bool canMatchClient(const QVector<NvClientDisplay>& displays, QString* reason = nullptr,
                           const QStringList& candidates = NvOutputTopology::qualifiedVirtualModes())
{
    QString layout;
    QStringList modes;
    QString error;
    const bool ok = NvOutputTopology::resolveClientDisplayLayout(displays, layout, modes, &error, nullptr,
                                                                 candidates);
    if (!ok && reason != nullptr) *reason = error;
    return ok;
}

// The display the client treats as main: the one at the desktop origin.
inline NvClientDisplay primaryDisplay(const QVector<NvClientDisplay>& displays)
{
    for (const NvClientDisplay& display : displays) {
        if (display.bounds.contains(QPoint(0, 0))) return display;
    }
    return displays.isEmpty() ? NvClientDisplay { QRect(0, 0, 1920, 1080), QSize(1920, 1080) } :
                                displays.first();
}

// First-connect proposal: match the client's displays whenever that works,
// otherwise one virtual display that fits the main display, scaled to fit.
inline Setup proposal(const QVector<NvClientDisplay>& displays,
                      const QStringList& candidates = NvOutputTopology::qualifiedVirtualModes())
{
    Setup setup;
    const bool canMatch = canMatchClient(displays, nullptr, candidates);
    QString layout;
    QStringList modes;
    if (canMatch) NvOutputTopology::resolveClientDisplayLayout(displays, layout, modes, nullptr, nullptr, candidates);
    const QString mode = canMatch ? modes.first() :
            suggestedMode(NvOutputTopology::clientMatchTarget(primaryDisplay(displays)), candidates);
    setup.hostLayout = layoutForChoice(canMatch ? MatchClient : SingleVirtual);
    setup.virtualMode1 = mode;
    setup.virtualMode2 = canMatch ? modes.value(1, mode) : mode;
    setup.scalingMode = QString::fromLatin1(NvOutputTopology::ScaledSpanMode);
    return setup;
}

// Connect time for a saved Match client setup: resolve the modes for the
// client displays as they are now and store them, so the dialog shows what
// is really used. Returns false (with a user-facing reason) only when the
// displays cannot be matched at all.
inline bool refreshMatchedModes(QSettings& settings, const QString& hostId, Setup& setup,
                                const QVector<NvClientDisplay>& displays, QString* reason = nullptr,
                                const QStringList& candidates = NvOutputTopology::qualifiedVirtualModes())
{
    if (setup.hostLayout != QLatin1String(NvOutputTopology::MatchClientHostLayout)) return true;
    QString layout;
    QStringList modes;
    QString error;
    if (!NvOutputTopology::resolveClientDisplayLayout(displays, layout, modes, &error, nullptr, candidates)) {
        if (reason != nullptr) *reason = error;
        return false;
    }
    const QString mode1 = modes.first();
    const QString mode2 = modes.value(1, mode1);
    if (setup.virtualMode1 != mode1 || setup.virtualMode2 != mode2) {
        setup.virtualMode1 = mode1;
        setup.virtualMode2 = mode2;
        save(settings, hostId, setup);
    }
    return true;
}

// A saved single or dual virtual layout whose mode this workstation does not
// accept (its flags from the last connect): the user-facing reason to ask
// again, or empty when the saved modes are fine or the layout is not virtual.
inline QString unsupportedModeReason(const Setup& setup, const QStringList& candidates)
{
    QStringList used;
    if (setup.hostLayout == QLatin1String(NvOutputTopology::SingleHostLayout)) {
        used = {setup.virtualMode1};
    } else if (setup.hostLayout == QLatin1String(NvOutputTopology::DualHorizontalHostLayout)) {
        used = {setup.virtualMode1, setup.virtualMode2};
    }
    for (const QString& mode : std::as_const(used)) {
        if (!candidates.contains(mode)) {
            return QCoreApplication::translate("RemoteDisplaySetup",
                                               "This workstation does not support %1. Choose another resolution.")
                    .arg(QString(mode).replace(QLatin1Char('x'), QChar(0x00D7)));
        }
    }
    return QString();
}

}
