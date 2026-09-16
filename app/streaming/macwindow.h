#pragma once

#include <SDL3/SDL.h>

namespace MacWindow {
void logGeometry(SDL_Window* window);
int unobscuredToolbarLeft(SDL_Window* window, int currentLeft, int toolbarWidth);
}
