#include "mackeyboardcapture.h"
#include "input/sdl-darwin-scancodes.h"

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <Carbon/Carbon.h>
#include <IOKit/hidsystem/ev_keymap.h>
#include <deque>
#include <utility>

namespace {
struct Modifier {
    SDL_Scancode scan;
    SDL_Keymod mod;
    CGEventFlags side;
    CGEventFlags otherSide;
    CGEventFlags either;
};
constexpr Modifier modifiers[] = {
    {SDL_SCANCODE_LSHIFT, SDL_KMOD_LSHIFT, NX_DEVICELSHIFTKEYMASK, NX_DEVICERSHIFTKEYMASK, kCGEventFlagMaskShift},
    {SDL_SCANCODE_RSHIFT, SDL_KMOD_RSHIFT, NX_DEVICERSHIFTKEYMASK, NX_DEVICELSHIFTKEYMASK, kCGEventFlagMaskShift},
    {SDL_SCANCODE_LCTRL, SDL_KMOD_LCTRL, NX_DEVICELCTLKEYMASK, NX_DEVICERCTLKEYMASK, kCGEventFlagMaskControl},
    {SDL_SCANCODE_RCTRL, SDL_KMOD_RCTRL, NX_DEVICERCTLKEYMASK, NX_DEVICELCTLKEYMASK, kCGEventFlagMaskControl},
    {SDL_SCANCODE_LALT, SDL_KMOD_LALT, NX_DEVICELALTKEYMASK, NX_DEVICERALTKEYMASK, kCGEventFlagMaskAlternate},
    {SDL_SCANCODE_RALT, SDL_KMOD_RALT, NX_DEVICERALTKEYMASK, NX_DEVICELALTKEYMASK, kCGEventFlagMaskAlternate},
    {SDL_SCANCODE_LGUI, SDL_KMOD_LGUI, NX_DEVICELCMDKEYMASK, NX_DEVICERCMDKEYMASK, kCGEventFlagMaskCommand},
    {SDL_SCANCODE_RGUI, SDL_KMOD_RGUI, NX_DEVICERCMDKEYMASK, NX_DEVICELCMDKEYMASK, kCGEventFlagMaskCommand},
};

SDL_Keymod keyModifiers(CGEventFlags flags)
{
    SDL_Keymod result = SDL_KMOD_NONE;
    for (unsigned i = 0; i < SDL_arraysize(modifiers); ++i) {
        const auto& m = modifiers[i];
        // Synthetic/device-independent flags have no side bits. Pick the left
        // side once, not two presses for the same aggregate modifier.
        bool pressed = (flags & m.either) && ((flags & m.side) ||
            (!(i & 1) && !(flags & (m.side | m.otherSide))));
        if (pressed) result |= m.mod;
    }
    if (flags & kCGEventFlagMaskAlphaShift) result |= SDL_KMOD_CAPS;
    return result;
}
}

struct MacKeyboardCapture::State
{
    std::function<bool()> ownsKeyboard;
    std::function<void()> releaseKeys;
    std::function<bool()> isTrusted = [] { return AXIsProcessTrusted(); };
    std::function<void()> requestTrust = [] {
        AXIsProcessTrustedWithOptions((CFDictionaryRef)@{(id)kAXTrustedCheckOptionPrompt: @YES});
    };
    CFMachPortRef tap = nullptr;
    CFRunLoopSourceRef source = nullptr;
    CFRunLoopTimerRef timer = nullptr;
    // Session has longstanding events at SDL_EVENT_USER itself. Reserve that
    // value if it is still available and always use a distinct registered ID.
    const Uint32 eventType = [] {
        const Uint32 base = SDL_RegisterEvents(2);
        return base ? base + 1 : 0;
    }();
    std::deque<SDL_KeyboardEvent> pending;
    SDL_Keymod lastModifiers = SDL_KMOD_NONE;
    bool active = false;
    bool dispatching = false;
    bool warned = false;
    bool wakeQueued = false;
    bool isoKeyboard = KBGetLayoutType(LMGetKbdType()) == kKeyboardISO;
    static constexpr unsigned QueueLimit = 256;

    void deactivate()
    {
        if (tap) CGEventTapEnable(tap, false);
        const bool wasActive = std::exchange(active, false);
        pending.clear();
        wakeQueued = false;
        if (eventType != 0) SDL_FlushEvent(eventType);
        lastModifiers = SDL_KMOD_NONE;
        if (wasActive) releaseKeys();
    }

    bool enqueue(SDL_Scancode scan, bool down, SDL_Keymod mods, Uint32 raw = 0)
    {
        if (pending.size() == QueueLimit) return false;
        if (!wakeQueued) {
            SDL_Event wake {};
            wake.type = eventType;
            if (!eventType || !SDL_PushEvent(&wake)) return false;
            wakeQueued = true;
        }
        SDL_KeyboardEvent key {};
        key.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        key.timestamp = SDL_GetTicksNS();
        key.down = down;
        key.scancode = scan;
        key.raw = raw;
        key.mod = mods;
        key.key = SDL_GetKeyFromScancode(scan, mods, true);
        pending.push_back(key);
        return true;
    }

    bool queueKey(CGEventType type, CGEventRef event)
    {
        const auto mods = keyModifiers(CGEventGetFlags(event));
        // Reconcile modifiers before every key, including keys held before
        // focus was gained. No parallel SDL/native path forwards the same key.
        for (const auto& m : modifiers) {
            if ((mods & m.mod) != (lastModifiers & m.mod) &&
                !enqueue(m.scan, (mods & m.mod) != 0, mods)) return false;
        }
        const bool capsChanged = (mods & SDL_KMOD_CAPS) != (lastModifiers & SDL_KMOD_CAPS);
        lastModifiers = mods;
        if (type == kCGEventFlagsChanged) {
            if (CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode) == kVK_CapsLock && capsChanged)
                return enqueue(SDL_SCANCODE_CAPSLOCK, true, mods) &&
                       enqueue(SDL_SCANCODE_CAPSLOCK, false, mods);
            return true;
        }
        // The Host owns key repeat. Do not fill the local queue with repeats.
        if (type == kCGEventKeyDown && CGEventGetIntegerValueField(event, kCGKeyboardEventAutorepeat))
            return true;
        const auto raw = CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);
        auto code = raw;
        if (isoKeyboard && (code == 10 || code == 50)) code = 60 - code;
        if (code < 0 || code >= SDL_arraysize(darwin_scancode_table) ||
            darwin_scancode_table[code] == SDL_SCANCODE_UNKNOWN) return false;
        return enqueue(darwin_scancode_table[code], type == kCGEventKeyDown, mods, raw);
    }

    CGEventRef handle(CGEventType type, CGEventRef event)
    {
        if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
            // Drop pending input and release the remote keyboard before the
            // timer rechecks permission/focus and attempts recovery.
            deactivate();
            SDL_LogWarn(SDL_LOG_CATEGORY_INPUT, "macOS shortcut capture interrupted; remote keys released");
            return event;
        }
        if (!active) return event;
        if (!ownsKeyboard()) {
            deactivate();
            return event;
        }
        if (type != kCGEventKeyDown && type != kCGEventKeyUp && type != kCGEventFlagsChanged)
            return event;
        // Fn and media/system-defined events remain local. Never intercept
        // pointing-device or Spaces gesture events.
        const auto raw = CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);
        const auto flags = CGEventGetFlags(event);
        if (raw == kVK_Escape && (flags & kCGEventFlagMaskCommand) &&
            (flags & kCGEventFlagMaskAlternate)) {
            // Keep the local emergency Force Quit chord available even if the
            // OS version happens to expose it to session taps.
            deactivate();
            return event;
        }
        if (raw == kVK_Function || raw == kVK_VolumeUp || raw == kVK_VolumeDown || raw == kVK_Mute)
            return event;
        if (!queueKey(type, event)) {
            deactivate();
            SDL_LogWarn(SDL_LOG_CATEGORY_INPUT, "Unable to queue macOS shortcut; capture released");
            return event;
        }
        return nullptr; // The existing PLANK input path, never the local OS.
    }

    static CGEventRef callback(CGEventTapProxy, CGEventType type, CGEventRef event, void* context)
    {
        @autoreleasepool { return static_cast<State*>(context)->handle(type, event); }
    }

    void refresh()
    {
        if (!ownsKeyboard()) { deactivate(); return; }
        if (!isTrusted()) {
            deactivate();
            // One OS permission prompt per app run, never from the tap itself.
            static bool prompted = false;
            if (!prompted) {
                prompted = true;
                requestTrust();
            }
            if (!warned) {
                SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                    "System shortcut capture needs PLANK Client Accessibility permission in System Settings > Privacy & Security. Local OS shortcuts remain active until approved.");
                warned = true;
            }
            return;
        }
        if (tap && !CFMachPortIsValid(tap)) {
            deactivate();
            if (source) {
                CFRunLoopRemoveSource(CFRunLoopGetMain(), source, kCFRunLoopCommonModes);
                CFRelease(source);
                source = nullptr;
            }
            CFRelease(tap);
            tap = nullptr;
        }
        if (active && tap && !CGEventTapIsEnabled(tap)) deactivate();
        if (!tap && eventType) {
            const CGEventMask mask = CGEventMaskBit(kCGEventKeyDown) |
                CGEventMaskBit(kCGEventKeyUp) | CGEventMaskBit(kCGEventFlagsChanged);
            tap = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap,
                                  kCGEventTapOptionDefault, mask, callback, this);
            if (tap) {
                source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap, 0);
                if (source) CFRunLoopAddSource(CFRunLoopGetMain(), source, kCFRunLoopCommonModes);
                else { CFMachPortInvalidate(tap); CFRelease(tap); tap = nullptr; }
            }
        }
        if (!tap) {
            if (!warned) SDL_LogWarn(SDL_LOG_CATEGORY_INPUT, "macOS system shortcut capture is unavailable");
            warned = true;
            return;
        }
        if (!active) {
            releaseKeys();
            lastModifiers = SDL_KMOD_NONE;
            active = true;
            CGEventTapEnable(tap, true);
            SDL_LogInfo(SDL_LOG_CATEGORY_INPUT, "macOS system shortcut capture enabled");
        }
        warned = false;
    }
};

MacKeyboardCapture::MacKeyboardCapture(std::function<bool()> ownsKeyboard,
                                       std::function<void()> releaseKeys)
    : MacKeyboardCapture(std::move(ownsKeyboard), std::move(releaseKeys), true) {}

MacKeyboardCapture::MacKeyboardCapture(std::function<bool()> ownsKeyboard,
                                       std::function<void()> releaseKeys, bool monitor)
    : m_State(std::make_unique<State>())
{
    m_State->ownsKeyboard = std::move(ownsKeyboard);
    m_State->releaseKeys = std::move(releaseKeys);
    if (monitor) {
        CFRunLoopTimerContext context {0, m_State.get(), nullptr, nullptr, nullptr};
        m_State->timer = CFRunLoopTimerCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent(), 1.0, 0, 0,
            [](CFRunLoopTimerRef, void* data) { @autoreleasepool { static_cast<State*>(data)->refresh(); } },
            &context);
        if (m_State->timer)
            CFRunLoopAddTimer(CFRunLoopGetMain(), m_State->timer, kCFRunLoopCommonModes);
    }
}

MacKeyboardCapture::~MacKeyboardCapture()
{
    NSCAssert(NSThread.isMainThread, @"Keyboard capture is main-thread only");
    if (m_State->timer) { CFRunLoopTimerInvalidate(m_State->timer); CFRelease(m_State->timer); }
    m_State->deactivate();
    if (m_State->source) {
        CFRunLoopRemoveSource(CFRunLoopGetMain(), m_State->source, kCFRunLoopCommonModes);
        CFRelease(m_State->source);
    }
    if (m_State->tap) { CFMachPortInvalidate(m_State->tap); CFRelease(m_State->tap); }
}

void MacKeyboardCapture::refresh()
{
    NSCAssert(NSThread.isMainThread, @"Keyboard capture is main-thread only");
    m_State->refresh();
}

bool MacKeyboardCapture::dispatch(const SDL_Event& event,
                                 const std::function<void(SDL_KeyboardEvent*)>& deliver)
{
    if (!m_State->eventType || event.type != m_State->eventType) return false;
    m_State->wakeQueued = false;
    while (!m_State->pending.empty()) {
        if (!m_State->active || !m_State->ownsKeyboard()) { m_State->deactivate(); break; }
        auto key = m_State->pending.front();
        m_State->pending.pop_front();
        m_State->dispatching = true;
        deliver(&key);
        m_State->dispatching = false;
    }
    return true;
}

bool MacKeyboardCapture::suppressSdlKeyEvent() const
{
    // The tap has already queued this input. Also exclude native key events
    // still in SDL's queue when capture began. Explicit menu/Dock Quit is not a key.
    return m_State->active && !m_State->dispatching;
}
