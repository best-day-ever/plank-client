# Render shutdown regression

This headless test links the actual Pacer queue/destructor and render/V-sync
workers against Qt, SDL3 and FFmpeg. A controlled renderer replaces GPU calls;
only the display-refresh query is stubbed. No networking, display server,
window, hardware decoding or installed Client is required.

On a qualified Client builder, select the same pinned Qt/FFmpeg/SDL inputs as
the product build, then run from an out-of-source build directory:

```sh
qmake "$PLANK_CLIENT_SOURCE/tests/pacershutdown/pacershutdown.pro" CONFIG+=release
make -j4
QT_QPA_PLATFORM=offscreen ./pacershutdown
```

Use `qmake6` where that is the qualified Qt executable. The common-C headers
default to the Client's pinned submodule; an isolated checkout can supply its
exact pinned header tree via `PLANK_COMMON_SOURCE=/path/to/common-source`.
Linux and macOS package builders run this test; it is not shipped in packages.

Coverage includes immediate/idle shutdown, shutdown during GPU readiness or an
active render, same-thread render-context cleanup, queued AVFrame release, and
asynchronous V-sync exit. The wait-boundary cases hold the real queue mutex
between checking the predicate and entering each condition wait, while the
real destructor attempts to stop. Moving the stop store outside that lock
(even with an atomic Boolean) must fail these cases. The synthetic waiter has
a bounded wait for failure reporting; the real idle render thread retains its
unbounded condition wait. Every case also has an independent hang watchdog.

The fix prevents a queue lost wakeup; it does not forcibly cancel a GPU driver
call that itself never returns. Real renderer, streaming and hardware
acceptance remain separate gates.
