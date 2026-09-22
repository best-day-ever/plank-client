#pragma once

// What AppKit knows about each display and CoreGraphics does not: the name
// System Settings shows, the panel's maximum refresh rate (built-in panels
// report 0 Hz through CoreGraphics), the camera-housing inset and whether
// "Displays have separate Spaces" is on. AppKit must be asked on the main
// thread, so refresh() fills a cache there and lookup() reads it from any
// thread (ClientDisplayProbe::probe runs on worker threads too).

#include <QString>

#include <cstdint>

namespace MacDisplayInfo {

struct Info
{
    bool valid = false;
    QString uuid;           // CGDisplayCreateUUIDFromDisplayID, uppercase
    QString name;           // NSScreen.localizedName
    int maximumFps = 0;     // NSScreen.maximumFramesPerSecond
    int safeAreaTop = 0;    // NSScreen.safeAreaInsets.top in points
};

// Re-reads every NSScreen. Main thread only; a no-op elsewhere.
void refresh();

// The cached entry for a CoreGraphics display id. On the main thread a
// missing entry triggers refresh(); elsewhere it stays invalid (the UUID is
// still filled: CoreGraphics answers from any thread).
Info lookup(uint32_t displayId);

// NSScreen.screensHaveSeparateSpaces, cached by refresh(); true until known.
bool screensHaveSeparateSpaces();

}
