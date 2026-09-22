#pragma once

// One description of the client's displays for everything that decides or
// shows what "Match client displays" does: the remote display dialog, the
// bookmark dialogs, the connect-time check and the streaming Session.
//
// Each display is an NvClientDisplay:
//   bounds      logical desktop coordinates (points on macOS)
//   nativeSize  physical panel pixels (macOS: the CoreGraphics native mode);
//               elsewhere the current desktop pixels
//   backingSize current desktop backing pixels (macOS only)
//   fullscreenSize the native-fullscreen viewport in backing pixels (macOS only):
//               the desktop minus the camera-housing safe area on notched panels
// NvOutputTopology::clientMatchTarget turns that into the size matched.
//
// The formatting helpers are header-only so the unit tests can exercise them.

#include "outputtopology.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QLocale>
#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <utility>

namespace ClientDisplayProbe {

// The active client displays sorted left to right, displays that mirror
// another one collapsed into it. macOS asks CoreGraphics (safe from any
// thread; names come from a cache MacDisplayInfo refreshes on the main
// thread); other platforms use QScreen (GUI thread only). Every display has
// a unique key (see uniqueKeys).
QVector<NvClientDisplay> probe();

// The Session's view of one SDL display, from the same probe the dialogs use:
// the probed entry with the same logical bounds gives the panel (nativeSize)
// and the desktop backing (backingSize). With no matching entry (no probe on
// this platform, or the display changed in between) the SDL native size is the
// panel and the desktop size is unknown. The Session resolves Match client and
// sizes the stream with exactly this, so the dialog and the stream cannot
// disagree about which size is the panel and which is the desktop.
inline NvClientDisplay forSessionDisplay(const QRect& sdlLogicalBounds, const QSize& sdlNativeSize,
                                         const QVector<NvClientDisplay>& probed)
{
    for (const NvClientDisplay& display : probed) {
        if (display.bounds == sdlLogicalBounds) {
            return display;
        }
    }
    return {sdlLogicalBounds, sdlNativeSize, QSize()};
}

// Stable identity of one monitor, independent of its size and position: the
// platform's display UUID where there is one, else vendor/model/serial, else
// the panel size (built-in or not) as a last resort.
inline QString monitorKey(const QString& uuid, quint32 vendor, quint32 model, quint32 serial,
                          bool builtIn, const QSize& panel)
{
    if (!uuid.trimmed().isEmpty()) {
        return QStringLiteral("uuid:") + uuid.trimmed().toUpper();
    }
    if (vendor != 0 || model != 0 || serial != 0) {
        return QStringLiteral("edid:%1-%2-%3").arg(vendor, 0, 16).arg(model, 0, 16).arg(serial, 0, 16);
    }
    return QStringLiteral("%1:%2x%3").arg(builtIn ? QStringLiteral("builtin") : QStringLiteral("panel"))
            .arg(panel.width()).arg(panel.height());
}

// Keys for a list of displays: each display's key, made unique with "#2",
// "#3" when two identical monitors report the same identity.
inline QStringList uniqueKeys(const QVector<NvClientDisplay>& displays)
{
    QStringList keys;
    for (const NvClientDisplay& display : displays) {
        const QString base = display.key.isEmpty() ?
                    monitorKey(QString(), 0, 0, 0, display.builtIn, display.nativeSize) : display.key;
        QString key = base;
        for (int copy = 2; keys.contains(key); ++copy) {
            key = base + QStringLiteral("#") + QString::number(copy);
        }
        keys.append(key);
    }
    return keys;
}

// Gives every display a unique key (see uniqueKeys).
inline void assignUniqueKeys(QVector<NvClientDisplay>& displays)
{
    const QStringList keys = uniqueKeys(displays);
    for (int index = 0; index < displays.size(); ++index) {
        displays[index].key = keys.at(index);
    }
}

// The monitor set: sha256 over the sorted keys, so the same monitors give the
// same fingerprint wherever they are placed and whatever resolution they run.
// 20 hex digits keep settings keys short and are ample for one user's desks.
inline QString fingerprint(const QVector<NvClientDisplay>& displays)
{
    if (displays.isEmpty()) return QString();
    QStringList keys = uniqueKeys(displays);
    keys.sort();
    return QString::fromLatin1(QCryptographicHash::hash(keys.join(QLatin1Char('\n')).toUtf8(),
                                                        QCryptographicHash::Sha256).toHex().left(20));
}

// A short human name for one monitor.
inline QString displayName(const NvClientDisplay& display)
{
    if (!display.name.trimmed().isEmpty()) return display.name.trimmed();
    return display.builtIn ? QCoreApplication::translate("ClientDisplayProbe", "Built-in display") :
                             QCoreApplication::translate("ClientDisplayProbe", "Display");
}

// "Built-in Retina Display + LG UltraFine": the monitor set, left to right.
inline QString label(const QVector<NvClientDisplay>& displays)
{
    QVector<NvClientDisplay> ordered = displays;
    std::stable_sort(ordered.begin(), ordered.end(), [](const NvClientDisplay& a, const NvClientDisplay& b) {
        return std::make_pair(a.bounds.x(), a.bounds.y()) < std::make_pair(b.bounds.x(), b.bounds.y());
    });
    QStringList names;
    for (const NvClientDisplay& display : std::as_const(ordered)) names.append(displayName(display));
    return names.join(QStringLiteral(" + "));
}

// The Mac-host Match client view of the displays (ComputerManager and the
// Session): each display's current desktop, or in fullscreen the viewport
// below the camera housing, whose logical bounds start below the inset.
inline QVector<NvClientDisplay> macMatchDisplays(const QVector<NvClientDisplay>& displays, bool fullscreen)
{
    QVector<NvClientDisplay> result;
    for (const NvClientDisplay& display : displays) {
        QRect bounds = display.bounds;
        QSize pixels = display.backingSize.isValid() ? display.backingSize : display.nativeSize;
        if (fullscreen && display.fullscreenSize.isValid() && display.backingSize.isValid() &&
                bounds.height() > 0 && display.backingSize.height() % bounds.height() == 0) {
            const int scale = display.backingSize.height() / bounds.height();
            const int top = (display.backingSize.height() - display.fullscreenSize.height()) / scale;
            bounds = QRect(bounds.x(), bounds.y() + top, bounds.width(), bounds.height() - top);
            pixels = display.fullscreenSize;
        }
        result.append({bounds, pixels, pixels});
    }
    return result;
}

inline QSize desktopPixels(const NvClientDisplay& display)
{
    return display.backingSize.isValid() ? display.backingSize : display.nativeSize;
}

// Desktop pixels per logical point (1 or 2 on macOS).
inline double scale(const NvClientDisplay& display)
{
    const QSize pixels = desktopPixels(display);
    return display.bounds.width() > 0 && pixels.isValid() ?
                double(pixels.width()) / display.bounds.width() : 1.0;
}

inline QString sizeText(const QSize& size)
{
    return QStringLiteral("%1 × %2").arg(size.width()).arg(size.height());
}

inline QString modeText(const QString& mode)
{
    QString text = mode;
    return text.replace(QLatin1Char('x'), QStringLiteral(" × "));
}

inline QString scaleText(double value)
{
    return QLocale().toString(value, 'g', 3) + QStringLiteral("×");
}

// "3024 × 1964 display, desktop 1920 × 1200 (1×)" when the desktop is not
// the panel's own pixels, "3024 × 1964 (2×)" for a Retina desktop at the
// panel's pixels, otherwise just "2560 × 1440".
inline QString describe(const NvClientDisplay& display)
{
    const QSize desktop = desktopPixels(display);
    const double pointScale = scale(display);
    if (display.backingSize.isValid() && display.nativeSize.isValid() && display.nativeSize != desktop) {
        return QCoreApplication::translate("ClientDisplayProbe", "%1 display, desktop %2 (%3)")
                .arg(sizeText(display.nativeSize), sizeText(desktop), scaleText(pointScale));
    }
    if (display.backingSize.isValid() && qAbs(pointScale - 1.0) > 0.01) {
        return QStringLiteral("%1 (%2)").arg(sizeText(desktop), scaleText(pointScale));
    }
    return sizeText(desktop);
}

inline QString describe(const QVector<NvClientDisplay>& displays)
{
    QStringList parts;
    for (const NvClientDisplay& display : displays) parts.append(describe(display));
    return parts.join(QStringLiteral(" + "));
}

struct MatchPreview
{
    bool ok = false;
    bool fitted = false;
    QString hostLayout;
    QStringList modes;
    QString reason;
};

inline MatchPreview matchPreview(const QVector<NvClientDisplay>& displays,
                                 const QStringList& candidateModes = NvOutputTopology::qualifiedVirtualModes())
{
    MatchPreview preview;
    preview.ok = NvOutputTopology::resolveClientDisplayLayout(displays, preview.hostLayout, preview.modes,
                                                              &preview.reason, &preview.fitted, candidateModes);
    return preview;
}

// "2560 × 1600 (closest supported size)" or "1920 × 1200 (exact)".
inline QString matchSummary(const MatchPreview& preview)
{
    if (!preview.ok) return QString();
    QStringList modes;
    for (const QString& mode : preview.modes) modes.append(modeText(mode));
    const QString joined = modes.join(QStringLiteral(" + "));
    return preview.fitted ?
                QCoreApplication::translate("ClientDisplayProbe", "%1 (closest supported size)").arg(joined) :
                QCoreApplication::translate("ClientDisplayProbe", "%1 (exact)").arg(joined);
}

// Not translated: one log line per display and connect.
inline QString logLine(const NvClientDisplay& display, const QString& matchMode, bool exact)
{
    const QSize panel = display.nativeSize;
    const QSize desktop = desktopPixels(display);
    const QSize target = NvOutputTopology::clientMatchTarget(display);
    return QStringLiteral("PLANK client display: panel=%1x%2 desktop=%3x%4@%5 target=%6x%7 match=%8 (%9)")
            .arg(panel.width()).arg(panel.height())
            .arg(desktop.width()).arg(desktop.height())
            .arg(QString::number(scale(display), 'g', 3))
            .arg(target.width()).arg(target.height())
            .arg(matchMode.isEmpty() ? QStringLiteral("-") : matchMode,
                 matchMode.isEmpty() ? QStringLiteral("unmatched") :
                                       exact ? QStringLiteral("exact") : QStringLiteral("fitted"));
}

}
