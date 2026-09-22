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
// NvOutputTopology::clientMatchTarget turns that into the size matched.
//
// The formatting helpers are header-only so the unit tests can exercise them.

#include "outputtopology.h"

#include <QCoreApplication>
#include <QLocale>
#include <QString>
#include <QStringList>
#include <QVector>

namespace ClientDisplayProbe {

// The active client displays sorted left to right. macOS asks CoreGraphics
// (safe from any thread); other platforms use QScreen (GUI thread only).
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
            return {sdlLogicalBounds, display.nativeSize, display.backingSize};
        }
    }
    return {sdlLogicalBounds, sdlNativeSize, QSize()};
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

inline MatchPreview matchPreview(const QVector<NvClientDisplay>& displays)
{
    MatchPreview preview;
    preview.ok = NvOutputTopology::resolveClientDisplayLayout(displays, preview.hostLayout, preview.modes,
                                                              &preview.reason, &preview.fitted);
    return preview;
}

// "2560 × 1600 (closest supported, letterboxed)" or "1920 × 1200 (exact)".
inline QString matchSummary(const MatchPreview& preview)
{
    if (!preview.ok) return QString();
    QStringList modes;
    for (const QString& mode : preview.modes) modes.append(modeText(mode));
    const QString joined = modes.join(QStringLiteral(" + "));
    return preview.fitted ?
                QCoreApplication::translate("ClientDisplayProbe", "%1 (closest supported, letterboxed)").arg(joined) :
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
