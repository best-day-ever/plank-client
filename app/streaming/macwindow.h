#pragma once

#include <SDL3/SDL.h>

namespace MacWindow {
bool fullscreenTopInset(Uint32 displayId, int* top);
void logGeometry(SDL_Window* window);
int unobscuredToolbarLeft(SDL_Window* window, int currentLeft, int toolbarWidth);
}
