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

#ifdef Q_OS_WIN32
#include <QCryptographicHash>

#include <vector>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

#ifdef Q_OS_WIN32
// What QueryDisplayConfig knows about each active monitor, by the GDI source
// name QScreen::name() reports ("\\.\DISPLAY1").
struct WindowsMonitor
{
    QString gdiName;
    QString name;
    QString key;
    bool builtIn = false;
};

QVector<WindowsMonitor> windowsMonitors()
{
    QVector<WindowsMonitor> monitors;
    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) {
        return monitors;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(),
                           nullptr) != ERROR_SUCCESS) {
        return monitors;
    }
    for (UINT32 index = 0; index < pathCount; ++index) {
        const DISPLAYCONFIG_PATH_INFO& path = paths[index];
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        DISPLAYCONFIG_TARGET_DEVICE_NAME target = {};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof(target);
        target.header.adapterId = path.targetInfo.adapterId;
        target.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
                DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS) {
            continue;
        }
        WindowsMonitor monitor;
        monitor.gdiName = QString::fromWCharArray(source.viewGdiDeviceName);
        monitor.name = QString::fromWCharArray(target.monitorFriendlyDeviceName);
        // The monitor's device path names the monitor and its connector;
        // hashed so settings keys stay short.
        const QString devicePath = QString::fromWCharArray(target.monitorDevicePath).toLower();
        if (!devicePath.isEmpty()) {
            monitor.key = QStringLiteral("win:") + QString::fromLatin1(
                        QCryptographicHash::hash(devicePath.toUtf8(), QCryptographicHash::Sha256).toHex().left(24));
        }
        const DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY technology = path.targetInfo.outputTechnology;
        monitor.builtIn = technology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL ||
                technology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED ||
                technology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED;
        // Clone mode: several targets share one source; the first one names it.
        bool known = false;
        for (const WindowsMonitor& other : std::as_const(monitors)) {
            known = known || other.gdiName == monitor.gdiName;
        }
        if (!known) monitors.append(monitor);
    }
    return monitors;
}
#endif

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
#ifdef Q_OS_WIN32
    const QVector<WindowsMonitor> monitors = windowsMonitors();
#endif
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
#ifdef Q_OS_WIN32
        for (const WindowsMonitor& monitor : monitors) {
            if (monitor.gdiName.compare(screen->name(), Qt::CaseInsensitive) != 0) continue;
            if (!monitor.key.isEmpty()) display.key = monitor.key;
            if (!monitor.name.isEmpty()) display.name = monitor.name;
            display.builtIn = monitor.builtIn;
            break;
        }
#endif
        displays.append(display);
    }
#endif
    std::sort(displays.begin(), displays.end(), [](const NvClientDisplay& a, const NvClientDisplay& b) {
        return std::make_pair(a.bounds.x(), a.bounds.y()) < std::make_pair(b.bounds.x(), b.bounds.y());
    });
    assignUniqueKeys(displays);
    return displays;
}
