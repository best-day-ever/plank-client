#include "macwindow.h"
#include "planktoolbarlogic.h"

#import <Cocoa/Cocoa.h>
#include <cmath>

bool MacWindow::fullscreenTopInset(Uint32 displayId, int* top)
{
    @autoreleasepool {
        for (NSScreen* screen in NSScreen.screens) {
            if ([screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue] == displayId) {
                // visibleFrame also excludes the Dock/menu bar: that is NOT
                // the native fullscreen viewport. Only reserve the camera area.
                *top = static_cast<int>(std::ceil(screen.safeAreaInsets.top));
                return true;
            }
        }
        return false;
    }
}

void MacWindow::logGeometry(SDL_Window* window)
{
    @autoreleasepool {
        NSWindow* nativeWindow = (__bridge NSWindow*)SDL_GetPointerProperty(
            SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
        if (!nativeWindow || !nativeWindow.screen)
            return;
        const NSRect panel = nativeWindow.screen.frame;
        const NSRect frame = nativeWindow.frame;
        const NSRect content = nativeWindow.contentView.bounds;
        int pixelWidth = 0, pixelHeight = 0;
        SDL_GetWindowSizeInPixels(window, &pixelWidth, &pixelHeight);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "PLANK Mac presentation: native-fullscreen=%d panel=%.0fx%.0f frame=%.0fx%.0f content=%.0fx%.0f drawable=%dx%d scale=%.2f",
                    (nativeWindow.styleMask & NSWindowStyleMaskFullScreen) != 0,
                    (double)panel.size.width, (double)panel.size.height,
                    (double)frame.size.width, (double)frame.size.height,
                    (double)content.size.width, (double)content.size.height,
                    pixelWidth, pixelHeight, (double)nativeWindow.backingScaleFactor);
    }
}

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
