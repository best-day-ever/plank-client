#include "clientdisplayprobe.h"

#include <algorithm>

#ifdef Q_OS_DARWIN
#include "streaming/macdisplaygeometry.h"
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
    for (uint32_t index = 0; index < count; ++index) {
        NvClientDisplay display;
        const CGRect bounds = CGDisplayBounds(ids[index]);
        display.bounds = QRect(qRound(bounds.origin.x), qRound(bounds.origin.y),
                               qRound(bounds.size.width), qRound(bounds.size.height));
        if (CGDisplayModeRef current = CGDisplayCopyDisplayMode(ids[index])) {
            display.backingSize = QSize(int(CGDisplayModeGetPixelWidth(current)),
                                        int(CGDisplayModeGetPixelHeight(current)));
            CGDisplayModeRelease(current);
        }
        // Native fullscreen sits below the camera housing: the viewport a fullscreen stream really gets.
        // Same geometry the Mac-host Match path uses (StreamUtils::getMacCurrentDisplayMode).
        int top = 0;
        int logicalHeight = display.bounds.height();
        int pixelHeight = display.backingSize.height();
        if (display.backingSize.isValid() && MacWindow::fullscreenTopInset(ids[index], &top) && top > 0 &&
                MacDisplayGeometry::insetTop(display.bounds.width(), logicalHeight,
                                             display.backingSize.width(), pixelHeight, top)) {
            display.fullscreenSize = QSize(display.backingSize.width(), pixelHeight);
        }
        display.nativeSize = nativePanelPixels(ids[index]);
        if (!display.nativeSize.isValid()) display.nativeSize = display.backingSize;
        if (!display.bounds.isValid() || !display.nativeSize.isValid()) continue;
        displays.append(display);
    }
#else
    for (QScreen* screen : QGuiApplication::screens()) {
        NvClientDisplay display;
        display.bounds = screen->geometry();
        display.nativeSize = QSize(qRound(screen->geometry().width() * screen->devicePixelRatio()),
                                   qRound(screen->geometry().height() * screen->devicePixelRatio()));
        displays.append(display);
    }
#endif
    std::sort(displays.begin(), displays.end(), [](const NvClientDisplay& a, const NvClientDisplay& b) {
        return std::make_pair(a.bounds.x(), a.bounds.y()) < std::make_pair(b.bounds.x(), b.bounds.y());
    });
    return displays;
}
