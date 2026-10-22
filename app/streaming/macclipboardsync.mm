#include "macclipboardsync.h"
#include "clipboardplatform.h"

#include <AppKit/AppKit.h>
#include <SDL3/SDL.h>

#ifdef PLANK_CLIPBOARD_TEST_PASTEBOARD
// Supplied only by the native test binary; never linked into the application.
extern NSPasteboard* plankClipboardTestPasteboard();
#endif

namespace {

NSPasteboard* clipboardPasteboard()
{
#ifdef PLANK_CLIPBOARD_TEST_PASTEBOARD
    return plankClipboardTestPasteboard();
#else
    return [NSPasteboard generalPasteboard];
#endif
}

NSArray<NSString*>* pasteboardTextTypes()
{
    return @[
        NSPasteboardTypeString,
        @"public.utf8-plain-text",
        @"public.plain-text",
    ];
}

std::string readGeneralPasteboardText()
{
    @autoreleasepool {
        NSPasteboard* pasteboard = clipboardPasteboard();
        if (pasteboard == nil) {
            return {};
        }
        NSDictionary* fileOptions = @{NSPasteboardURLReadingFileURLsOnlyKey: @YES};
        if ([pasteboard canReadObjectForClasses:@[[NSURL class]] options:fileOptions]) {
            return {};
        }
        for (NSString* type in pasteboardTextTypes()) {
            NSString* text = [pasteboard stringForType:type];
            if (text != nil && text.length > 0) {
                const auto size = [text lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
                if (size > PLANK_CLIPBOARD_MAX_TEXT_SIZE) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Clipboard text exceeds 512 KiB; not shared");
                    return {};
                }
                NSData* utf8 = [text dataUsingEncoding:NSUTF8StringEncoding allowLossyConversion:NO];
                if (plank_clipboard_valid_text(static_cast<const uint8_t*>(utf8.bytes), utf8.length))
                    return std::string(static_cast<const char*>(utf8.bytes), utf8.length);
                return {};
            }
        }
    }
    return {};
}

bool writeGeneralPasteboardText(const std::vector<std::uint8_t>& bytes)
{
    if (bytes.empty()) {
        return false;
    }
    @autoreleasepool {
        NSString* text = [[NSString alloc] initWithBytes:bytes.data()
                                                  length:bytes.size()
                                                encoding:NSUTF8StringEncoding];
        if (text == nil) {
            return false;
        }
        NSPasteboard* pasteboard = clipboardPasteboard();
        [pasteboard clearContents];
        const bool written = [pasteboard setString:text forType:NSPasteboardTypeString];
        [text release];
        return written;
    }
}

int currentPasteboardChangeCount()
{
    @autoreleasepool {
        NSPasteboard* pasteboard = clipboardPasteboard();
        return pasteboard != nil ? pasteboard.changeCount : -1;
    }
}

} // namespace

namespace ClipboardPlatform {
std::optional<std::string> readText() { return readGeneralPasteboardText(); }
bool writeText(const std::vector<std::uint8_t>& bytes) { return writeGeneralPasteboardText(bytes); }
std::int64_t changeCount() { return currentPasteboardChangeCount(); }
void clearIfMatches(std::int64_t sequence, const std::string& text)
{
    @autoreleasepool {
        NSPasteboard* pasteboard = clipboardPasteboard();
        if (pasteboard != nil && pasteboard.changeCount == sequence &&
                readGeneralPasteboardText() == text && pasteboard.changeCount == sequence) {
            [pasteboard clearContents];
        }
    }
}
void runOnMainThread(std::function<void()> callback)
{
    if ([NSThread isMainThread]) callback();
    else dispatch_async(dispatch_get_main_queue(), ^{ callback(); });
}
} // namespace ClipboardPlatform

#ifdef Q_OS_MACOS
char* macReadGeneralPasteboardTextForSdl()
{
    const std::string text = readGeneralPasteboardText();
    return text.empty() ? nullptr : SDL_strdup(text.c_str());
}
#endif
