#include <QtTest>

#include <AppKit/AppKit.h>

#include "macclipboardsync.h"

class TestMacClipboardSync : public QObject
{
    Q_OBJECT

private:
    static void setPasteboardText(const char* text)
    {
        @autoreleasepool {
            NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
            [pasteboard clearContents];
            [pasteboard setString:[NSString stringWithUTF8String:text]
                         forType:NSPasteboardTypeString];
        }
    }

    static std::vector<std::uint8_t> oneFrame(const char* text,
                                              std::uint64_t generation)
    {
        const auto frames = plank::clipboard::buildEventFrames(
                    reinterpret_cast<const std::uint8_t*>(text),
                    std::strlen(text),
                    generation,
                    PLANK_CLIPBOARD_MAX_EVENT_CHUNK_SIZE);
        Q_ASSERT(frames.size() == 1);
        return frames.front();
    }

private slots:
    void rejectsMalformedFrameLength();
    void acceptsIndependentDirectionGenerations();
    void resetsGenerationAndRejectsStaleEventsOnReconnect();
    void suppressesRepeatedHostText();
    void sendsOnlyWithStreamFocus();
    void retriesAfterTransportSendFailure();
    void rejectsHostOfferWhenEventQueueFails();
    void ignoresOffersWhileStopped();
    void validatesUnicodeScalars();
};

void TestMacClipboardSync::rejectsMalformedFrameLength()
{
    int queuedEvents = 0;
    MacClipboardSync sync(
                [](const std::uint8_t*, std::size_t) { return true; },
                [] { return true; },
                [] { return true; },
                [&] {
                    ++queuedEvents;
                    return true;
                });
    sync.start();

    auto truncated = oneFrame("frame", 1);
    truncated.pop_back();
    QVERIFY(!sync.handleHostOffer(truncated.data(), truncated.size()));
    QCOMPARE(queuedEvents, 0);

    auto trailing = oneFrame("frame", 2);
    trailing.push_back(0);
    QVERIFY(!sync.handleHostOffer(trailing.data(), trailing.size()));
    QCOMPARE(queuedEvents, 0);
}

void TestMacClipboardSync::acceptsIndependentDirectionGenerations()
{
    std::vector<std::vector<std::uint8_t>> sent;
    int queuedEvents = 0;
    MacClipboardSync sync(
                [&](const std::uint8_t* data, std::size_t size) {
                    sent.emplace_back(data, data + size);
                    return true;
                },
                [] { return true; },
                [] { return true; },
                [&] {
                    ++queuedEvents;
                    return true;
                });
    sync.start();

    setPasteboardText("client generation one");
    sync.pollLocalClipboardOnMainThread();
    QCOMPARE(sent.size(), std::size_t {1});

    PLANK_CLIPBOARD_WIRE_HEADER outbound {};
    std::memcpy(&outbound, sent.front().data(), sizeof(outbound));
    QCOMPARE(qFromLittleEndian(outbound.generation), std::uint64_t {1});

    const auto host = oneFrame("host generation one", 1);
    QVERIFY(sync.handleHostOffer(host.data(), host.size()));
    QCOMPARE(queuedEvents, 1);
    QVERIFY(sync.applyPendingHostTextOnMainThread());
}

void TestMacClipboardSync::resetsGenerationAndRejectsStaleEventsOnReconnect()
{
    int queuedEvents = 0;
    MacClipboardSync sync(
                [](const std::uint8_t*, std::size_t) { return true; },
                [] { return true; },
                [] { return true; },
                [&] {
                    ++queuedEvents;
                    return true;
                });
    sync.start();

    const auto oldSession = oneFrame("old session", 42);
    QVERIFY(sync.handleHostOffer(oldSession.data(), oldSession.size()));
    QCOMPARE(queuedEvents, 1);

    sync.stop();
    sync.start();
    QVERIFY(!sync.applyPendingHostTextOnMainThread());

    const auto newSession = oneFrame("new session", 1);
    QVERIFY(sync.handleHostOffer(newSession.data(), newSession.size()));
    QCOMPARE(queuedEvents, 2);
    QVERIFY(sync.applyPendingHostTextOnMainThread());
}

void TestMacClipboardSync::suppressesRepeatedHostText()
{
    int queuedEvents = 0;
    MacClipboardSync sync(
                [](const std::uint8_t*, std::size_t) { return true; },
                [] { return true; },
                [] { return true; },
                [&] {
                    ++queuedEvents;
                    return true;
                });
    sync.start();

    auto frame = oneFrame("same text", 1);
    QVERIFY(sync.handleHostOffer(frame.data(), frame.size()));
    QCOMPARE(queuedEvents, 1);

    frame = oneFrame("same text", 2);
    QVERIFY(sync.handleHostOffer(frame.data(), frame.size()));
    QCOMPARE(queuedEvents, 1);
    QVERIFY(sync.applyPendingHostTextOnMainThread());

    frame = oneFrame("same text", 3);
    QVERIFY(sync.handleHostOffer(frame.data(), frame.size()));
    QCOMPARE(queuedEvents, 1);
}

void TestMacClipboardSync::sendsOnlyWithStreamFocus()
{
    bool focused = false;
    std::vector<std::vector<std::uint8_t>> sent;
    MacClipboardSync sync(
                [&](const std::uint8_t* data, std::size_t size) {
                    sent.emplace_back(data, data + size);
                    return true;
                },
                [&] { return focused; },
                [] { return true; },
                [] { return true; });
    sync.start();

    setPasteboardText("focus gated");
    sync.pollLocalClipboardOnMainThread();
    QVERIFY(sent.empty());

    focused = true;
    sync.pollLocalClipboardOnMainThread();
    QCOMPARE(sent.size(), std::size_t {1});
}

void TestMacClipboardSync::retriesAfterTransportSendFailure()
{
    int sends = 0;
    MacClipboardSync sync(
                [&](const std::uint8_t*, std::size_t) {
                    return ++sends > 1;
                },
                [] { return true; },
                [] { return true; },
                [] { return true; });
    sync.start();

    setPasteboardText("retry send");
    sync.pollLocalClipboardOnMainThread();
    QCOMPARE(sends, 1);
    sync.pollLocalClipboardOnMainThread();
    QCOMPARE(sends, 2);
}

void TestMacClipboardSync::rejectsHostOfferWhenEventQueueFails()
{
    MacClipboardSync sync(
                [](const std::uint8_t*, std::size_t) { return true; },
                [] { return true; },
                [] { return true; },
                [] { return false; });
    sync.start();

    const auto frame = oneFrame("queue failure", 1);
    QVERIFY(!sync.handleHostOffer(frame.data(), frame.size()));
    QVERIFY(!sync.applyPendingHostTextOnMainThread());
}

void TestMacClipboardSync::ignoresOffersWhileStopped()
{
    int queuedEvents = 0;
    MacClipboardSync sync(
                [](const std::uint8_t*, std::size_t) { return true; },
                [] { return true; },
                [] { return true; },
                [&] {
                    ++queuedEvents;
                    return true;
                });
    sync.start();
    sync.stop();

    const auto frame = oneFrame("after stop", 1);
    QVERIFY(sync.handleHostOffer(frame.data(), frame.size()));
    QCOMPARE(queuedEvents, 0);
}

void TestMacClipboardSync::validatesUnicodeScalars()
{
    const char valid[] = "\xF0\x9F\x94\xA5";
    QVERIFY(plank::clipboard::validUtf8(valid, sizeof(valid) - 1));

    const char overlong[] = "\xC0\xAF";
    QVERIFY(!plank::clipboard::validUtf8(overlong, sizeof(overlong) - 1));

    const char surrogate[] = "\xED\xA0\x80";
    QVERIFY(!plank::clipboard::validUtf8(surrogate, sizeof(surrogate) - 1));

    const char outOfRange[] = "\xF4\x90\x80\x80";
    QVERIFY(!plank::clipboard::validUtf8(outOfRange, sizeof(outOfRange) - 1));

    const char embeddedNull[] = {'a', '\0', 'b'};
    QVERIFY(!plank::clipboard::validUtf8(embeddedNull, sizeof(embeddedNull)));
}

QTEST_MAIN(TestMacClipboardSync)

#include "test_macclipboardsync.moc"
