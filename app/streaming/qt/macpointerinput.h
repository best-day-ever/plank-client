#pragma once

#include "sessiongeometry.h"
#import <AppKit/AppKit.h>

inline bool qtMacPointerIsTablet(NSEvent* event)
{
    return event.type == NSEventTypeTabletPoint || event.subtype == NSEventSubtypeTabletPoint;
}

inline bool qtMacDiscardStalePointer(NSEvent* event, NSWindow* window, NSTimeInterval cutoff)
{
    // Tablet releases must survive a mouse capture handoff even if queued.
    return !qtMacPointerIsTablet(event) && event.window == window && event.timestamp <= cutoff;
}

inline bool qtMacUnacceleratedPointerDelta(NSEvent* event, const QtSessionGeometry& g, QPointF& delta)
{
    CGEventRef raw = event.CGEvent;
    if (!raw || !g.valid()) return false;
    // NSEvent.deltaX/Y include system acceleration. Quartz exposes the original
    // pointer displacement separately; never infer it from cursor positions.
    const double x = CGEventGetDoubleValueField(raw, kCGEventUnacceleratedPointerMovementX);
    const double y = CGEventGetDoubleValueField(raw, kCGEventUnacceleratedPointerMovementY);
    delta = QPointF(x * g.drawableSize.width() / g.logicalSize.width() *
                       g.sourcePixels.width() / g.destinationPixels.width(),
                    y * g.drawableSize.height() / g.logicalSize.height() *
                       g.sourcePixels.height() / g.destinationPixels.height());
    return std::isfinite(delta.x()) && std::isfinite(delta.y());
}
