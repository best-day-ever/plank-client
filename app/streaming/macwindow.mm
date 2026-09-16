#include "macwindow.h"
#include "planktoolbarlogic.h"

#import <Cocoa/Cocoa.h>
#include <cmath>

int MacWindow::unobscuredToolbarLeft(SDL_Window* window, int currentLeft, int toolbarWidth)
{
    @autoreleasepool {
        if (!(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN))
            return currentLeft;
        NSWindow* nativeWindow = (__bridge NSWindow*)SDL_GetPointerProperty(
            SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
        NSScreen* screen = nativeWindow.screen;
        if (!screen || screen.safeAreaInsets.top <= 0)
            return currentLeft;

        // AppKit exposes the actual unobscured areas: do not assume a specific
        // laptop model, notch width, desktop scale or global screen origin.
        const NSRect left = screen.auxiliaryTopLeftArea;
        const NSRect right = screen.auxiliaryTopRightArea;
        const CGFloat origin = nativeWindow.frame.origin.x;
        int width = 0;
        if (!SDL_GetWindowSize(window, &width, nullptr))
            return currentLeft;
        return PlankToolbarLogic::unobscuredToolbarLeft(
            currentLeft, toolbarWidth, width,
            static_cast<int>(std::floor(NSMaxX(left) - origin)),
            static_cast<int>(std::ceil(NSMinX(right) - origin)));
    }
}
