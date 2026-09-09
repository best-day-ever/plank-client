// Synthetic SDL boundary: no compositor, display or device initialization.
#include <SDL3/SDL.h>
#include <cassert>
#include <cstdio>

struct SDL_Cursor { int identity; } normal{1}, blank{2};
static SDL_Cursor* current = &normal;
static SDL_Surface surface;
static bool allocationFails;
static int created, destroyed, selected, shown;
SDL_Surface* SDL_CreateSurface(int width, int height, SDL_PixelFormat format) {
    assert(width == 1 && height == 1 && format == SDL_PIXELFORMAT_RGBA8888);
    return allocationFails ? nullptr : &surface;
}
bool SDL_FillSurfaceRect(SDL_Surface* value, const SDL_Rect* rect, Uint32 color) {
    assert(value == &surface && rect == nullptr && color == 0); return true;
}
SDL_Cursor* SDL_CreateColorCursor(SDL_Surface* value, int x, int y) {
    assert(value == &surface && x == 0 && y == 0); ++created; return &blank;
}
void SDL_DestroySurface(SDL_Surface* value) { assert(value == &surface); }
SDL_Cursor* SDL_GetCursor() { return current; }
SDL_Cursor* SDL_GetDefaultCursor() { return &normal; }
bool SDL_SetCursor(SDL_Cursor* value) { current = value; ++selected; return true; }
bool SDL_ShowCursor() { ++shown; return true; }
void SDL_DestroyCursor(SDL_Cursor* value) { assert(value == &blank && current != value); ++destroyed; }

#include "streaming/plankembeddedcursor.h"
int main() {
    { PlankEmbeddedCursor unused; } // Linux path: no cursor/API changes.
    assert(created == 0 && selected == 0 && shown == 0);
    {
        PlankEmbeddedCursor cursor;
        assert(cursor.setVisible(true) && created == 0);
        allocationFails = true;
        assert(!cursor.setVisible(false) && current == &normal);
        allocationFails = false;
        for (int i = 0; i < 100; ++i) {
            assert(cursor.setVisible(false) && current == &blank);
            int before = selected;
            assert(cursor.setVisible(false) && selected == before);
            assert(cursor.setVisible(true) && current == &normal);
        }
        assert(created == 1);
        assert(cursor.setVisible(false)); // teardown while still hidden
    }
    assert(current == &normal && destroyed == 1 && shown == 1);
    puts("embedded_cursor_pass cycles=100 transparent=1 restore=1 bounded=1 unused_noop=1");
}
