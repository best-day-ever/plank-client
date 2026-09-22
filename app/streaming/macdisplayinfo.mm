#include "macdisplayinfo.h"

#import <Cocoa/Cocoa.h>
#include <ApplicationServices/ApplicationServices.h>

#include <QHash>
#include <QMutex>
#include <QMutexLocker>

#include <atomic>
#include <cmath>

namespace {

QMutex s_Lock;
QHash<uint32_t, MacDisplayInfo::Info> s_Cache;
std::atomic<bool> s_SeparateSpaces {true};

QString displayUuid(uint32_t displayId)
{
    CFUUIDRef uuid = CGDisplayCreateUUIDFromDisplayID(displayId);
    if (uuid == nullptr) return QString();
    CFStringRef text = CFUUIDCreateString(kCFAllocatorDefault, uuid);
    CFRelease(uuid);
    if (text == nullptr) return QString();
    const QString result = QString::fromCFString(text).toUpper();
    CFRelease(text);
    return result;
}

}

void MacDisplayInfo::refresh()
{
    if (![NSThread isMainThread]) return;
    @autoreleasepool {
        QHash<uint32_t, Info> fresh;
        for (NSScreen* screen in NSScreen.screens) {
            const uint32_t displayId = [screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
            Info info;
            info.valid = true;
            info.uuid = displayUuid(displayId);
            info.name = QString::fromNSString(screen.localizedName);
            info.maximumFps = static_cast<int>(screen.maximumFramesPerSecond);
            info.safeAreaTop = static_cast<int>(std::ceil(screen.safeAreaInsets.top));
            fresh.insert(displayId, info);
        }
        s_SeparateSpaces.store(NSScreen.screensHaveSeparateSpaces);
        QMutexLocker lock(&s_Lock);
        s_Cache = fresh;
    }
}

MacDisplayInfo::Info MacDisplayInfo::lookup(uint32_t displayId)
{
    {
        QMutexLocker lock(&s_Lock);
        const auto entry = s_Cache.constFind(displayId);
        if (entry != s_Cache.constEnd()) return entry.value();
    }
    if ([NSThread isMainThread]) {
        refresh();
        QMutexLocker lock(&s_Lock);
        const auto entry = s_Cache.constFind(displayId);
        if (entry != s_Cache.constEnd()) return entry.value();
    }
    Info partial;
    partial.uuid = displayUuid(displayId);
    return partial;
}

bool MacDisplayInfo::screensHaveSeparateSpaces()
{
    return s_SeparateSpaces.load();
}
