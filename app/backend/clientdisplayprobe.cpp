#include "clientdisplayprobe.h"

#include <algorithm>
#include <cmath>

#ifdef Q_OS_DARWIN
#include "streaming/macdisplaygeometry.h"
#include "streaming/macdisplayinfo.h"
#include "streaming/macwindow.h"

#include <ApplicationServices/ApplicationServices.h>
#else
#include <QGuiApplication>
#include <QScreen>
#endif

namespace {

#ifdef Q_OS_DARWIN
QSize nativePanelPixels(CGDirectDisplayID displayId)
{
    QSize native;
    CFArrayRef modes = CGDisplayCopyAllDisplayModes(displayId, nullptr);
    if (modes == nullptr) return native;
    for (CFIndex index = 0; index < CFArrayGetCount(modes); ++index) {
        auto mode = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, index);
        if ((CGDisplayModeGetIOFlags(mode) & kDisplayModeNativeFlag) != 0) {
            native = QSize(int(CGDisplayModeGetPixelWidth(mode)), int(CGDisplayModeGetPixelHeight(mode)));
            break;
        }
    }
    CFRelease(modes);
    return native;
}
#endif

}

QVector<NvClientDisplay> ClientDisplayProbe::probe()
{
    QVector<NvClientDisplay> displays;
#ifdef Q_OS_DARWIN
    CGDirectDisplayID ids[16];
    uint32_t count = 0;
    if (CGGetActiveDisplayList(16, ids, &count) != kCGErrorSuccess) return displays;
    const CGDirectDisplayID mainDisplay = CGMainDisplayID();
    for (uint32_t index = 0; index < count; ++index) {
        // A display mirroring another shows the same desktop: collapse it
        // into the display it mirrors (which is also in this list).
        if (CGDisplayMirrorsDisplay(ids[index]) != kCGNullDirectDisplay) continue;
        NvClientDisplay display;
        const CGRect bounds = CGDisplayBounds(ids[index]);
        display.bounds = QRect(qRound(bounds.origin.x), qRound(bounds.origin.y),
                               qRound(bounds.size.width), qRound(bounds.size.height));
        if (CGDisplayModeRef current = CGDisplayCopyDisplayMode(ids[index])) {
            display.backingSize = QSize(int(CGDisplayModeGetPixelWidth(current)),
                                        int(CGDisplayModeGetPixelHeight(current)));
            display.refreshMillihz = int(std::lround(CGDisplayModeGetRefreshRate(current) * 1000.0));
            CGDisplayModeRelease(current);
        }
        const MacDisplayInfo::Info info = MacDisplayInfo::lookup(ids[index]);
        // Native fullscreen sits below the camera housing: the viewport a fullscreen stream really gets.
        // Same geometry the Mac-host Match path uses (StreamUtils::getMacCurrentDisplayMode).
        int top = 0;
        int logicalHeight = display.bounds.height();
        int pixelHeight = display.backingSize.height();
        if (display.backingSize.isValid() && MacWindow::fullscreenTopInset(ids[index], &top) && top > 0 &&
                MacDisplayGeometry::insetTop(display.bounds.width(), logicalHeight,
                                             display.backingSize.width(), pixelHeight, top)) {
            display.fullscreenSize = QSize(display.backingSize.width(), pixelHeight);
            display.notch = true;
        }
        display.nativeSize = nativePanelPixels(ids[index]);
        if (!display.nativeSize.isValid()) display.nativeSize = display.backingSize;
        if (!display.bounds.isValid() || !display.nativeSize.isValid()) continue;
        display.builtIn = CGDisplayIsBuiltin(ids[index]);
        display.main = ids[index] == mainDisplay;
        display.mirrored = CGDisplayIsInMirrorSet(ids[index]);
        display.rotation = int(std::lround(CGDisplayRotation(ids[index]))) % 360;
        // Built-in panels report 0 Hz through CoreGraphics; AppKit knows the panel's rate.
        if (display.refreshMillihz <= 0 && info.maximumFps > 0) display.refreshMillihz = info.maximumFps * 1000;
        display.name = info.name;
        display.platformId = ids[index];
        display.key = monitorKey(info.uuid, CGDisplayVendorNumber(ids[index]), CGDisplayModelNumber(ids[index]),
                                 CGDisplaySerialNumber(ids[index]), display.builtIn, display.nativeSize);
        displays.append(display);
    }
#else
    const QScreen* primary = QGuiApplication::primaryScreen();
    for (QScreen* screen : QGuiApplication::screens()) {
        NvClientDisplay display;
        display.bounds = screen->geometry();
        display.nativeSize = QSize(qRound(screen->geometry().width() * screen->devicePixelRatio()),
                                   qRound(screen->geometry().height() * screen->devicePixelRatio()));
        display.name = screen->model().isEmpty() ? screen->name() : screen->model();
        display.main = screen == primary;
        display.refreshMillihz = int(std::lround(screen->refreshRate() * 1000.0));
        const QString identity = QStringList {screen->manufacturer(), screen->model(), screen->serialNumber()}
                .join(QLatin1Char('/'));
        display.key = identity == QLatin1String("//") ?
                    monitorKey(QString(), 0, 0, 0, false, display.nativeSize) :
                    QStringLiteral("qt:") + identity;
        displays.append(display);
    }
#endif
    std::sort(displays.begin(), displays.end(), [](const NvClientDisplay& a, const NvClientDisplay& b) {
        return std::make_pair(a.bounds.x(), a.bounds.y()) < std::make_pair(b.bounds.x(), b.bounds.y());
    });
    assignUniqueKeys(displays);
    return displays;
}
