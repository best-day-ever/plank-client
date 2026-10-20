#include <QtTest>
#include "clipboardsync.h"
#include "clipboardplatform.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <QGuiApplication>
#include <QProcess>

class TestWindowsClipboardSync : public QObject {
    Q_OBJECT
    static bool setText(const std::string& text) {
        return ClipboardPlatform::writeText({text.begin(), text.end()});
    }
    static std::vector<uint8_t> offer(const std::string& text, uint64_t generation = 1) {
        return plank::clipboard::buildEventFrames(reinterpret_cast<const uint8_t*>(text.data()),
            text.size(), generation, PLANK_CLIPBOARD_MAX_EVENT_CHUNK_SIZE).front();
    }
private slots:
    void unicodeRoundTrip() {
        const std::string text = u8"Windows ↔ Linux: café 日本語 🖊\nline two";
        QVERIFY(setText(text));
        auto read = ClipboardPlatform::readText();
        QVERIFY(read.has_value());
        QCOMPARE(*read, text);
        QVERIFY(ClipboardPlatform::changeCount() >= 0);
    }
    void nativeCopySendsOnceAndHostCopyDoesNotEcho() {
        std::vector<std::vector<uint8_t>> sent;
        ClipboardSync sync([&](const uint8_t* data, size_t size) {
            sent.emplace_back(data, data+size); return true;
        }, [] { return true; }, [] { return true; }, [] { return true; });
        sync.start();
        QVERIFY(setText("local"));
        QVERIFY(sync.pollLocalClipboardOnMainThread());
        QVERIFY(sync.pollLocalClipboardOnMainThread());
        QCOMPARE(sent.size(), size_t(1));
        auto remote = offer("remote");
        QVERIFY(sync.handleHostOffer(remote.data(), remote.size()));
        QVERIFY(sync.applyPendingHostTextOnMainThread());
        QCOMPARE(*ClipboardPlatform::readText(), std::string("remote"));
        QVERIFY(sync.pollLocalClipboardOnMainThread());
        QCOMPARE(sent.size(), size_t(1));
        sync.stop();
        QCOMPARE(*ClipboardPlatform::readText(), std::string{});
    }
    void permissionAndFocusAreRequired() {
        bool focus = false, enabled = true;
        unsigned sent = 0;
        ClipboardSync sync([&](const uint8_t*, size_t) { ++sent; return true; },
            [&] { return focus; }, [&] { return enabled; }, [] { return true; });
        sync.start();
        QVERIFY(setText("private local copy"));
        sync.pollLocalClipboardOnMainThread();
        QCOMPARE(sent, 0u);
        focus = true; enabled = false;
        sync.pollLocalClipboardOnMainThread();
        QCOMPARE(sent, 0u);
        auto remote = offer("host");
        QVERIFY(sync.handleHostOffer(remote.data(), remote.size()));
        QVERIFY(!sync.applyPendingHostTextOnMainThread());
        QCOMPARE(*ClipboardPlatform::readText(), std::string("private local copy"));
        enabled = true;
        sync.pollLocalClipboardOnMainThread();
        QCOMPARE(sent, 1u);
    }
    void deferredPasteResumesAfterBackpressure() {
        bool accept = false;
        unsigned sent = 0;
        ClipboardSync sync([&](const uint8_t*, size_t) { if (!accept) return false; ++sent; return true; },
            [] { return true; }, [] { return true; }, [] { return true; });
        sync.start();
        QVERIFY(setText(std::string(20000, 'x')));
        QVERIFY(!sync.pollLocalClipboardOnMainThread());
        QCOMPARE(sent, 0u);
        accept = true;
        QVERIFY(sync.pollLocalClipboardOnMainThread());
        QVERIFY(sent > 1u);
        const auto count = sent;
        QVERIFY(sync.pollLocalClipboardOnMainThread());
        QCOMPARE(sent, count);
    }
    void disconnectPreservesNewLocalCopy() {
        ClipboardSync sync([](const uint8_t*, size_t) { return true; },
            [] { return true; }, [] { return true; }, [] { return true; });
        sync.start();
        auto remote = offer("remote");
        QVERIFY(sync.handleHostOffer(remote.data(), remote.size()));
        QVERIFY(sync.applyPendingHostTextOnMainThread());
        QVERIFY(setText("new local copy"));
        sync.stop();
        QCOMPARE(*ClipboardPlatform::readText(), std::string("new local copy"));
    }
    void reconnectDropsPendingOldSession() {
        ClipboardSync sync([](const uint8_t*, size_t) { return true; },
            [] { return true; }, [] { return true; }, [] { return true; });
        QVERIFY(setText("local"));
        sync.start();
        auto remote = offer("old host");
        QVERIFY(sync.handleHostOffer(remote.data(), remote.size()));
        sync.stop(); sync.start();
        QVERIFY(!sync.applyPendingHostTextOnMainThread());
        QCOMPARE(*ClipboardPlatform::readText(), std::string("local"));
    }
    void busyClipboardRetriesReadsAndHostPublication() {
        ClipboardSync sync([](const uint8_t*, size_t) { return true; },
            [] { return true; }, [] { return true; }, [] { return true; });
        QVERIFY(setText("local before lock"));
        sync.start();
        auto remote = offer("host after lock");
        QVERIFY(sync.handleHostOffer(remote.data(), remote.size()));
        QProcess holder;
        holder.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--hold-clipboard")});
        const bool acquired = holder.waitForReadyRead(5000) && holder.readLine().trimmed() == "LOCKED";
        const auto read = ClipboardPlatform::readText();
        const bool applied = sync.applyPendingHostTextOnMainThread();
        holder.write("release\n"); holder.closeWriteChannel();
        holder.waitForFinished(5000);
        QVERIFY(acquired);
        QVERIFY(!read);
        QVERIFY(!applied);
        // A poll retries the pending Host offer instead of silently losing it.
        QVERIFY(sync.pollLocalClipboardOnMainThread());
        QCOMPARE(*ClipboardPlatform::readText(), std::string("host after lock"));
    }
    void rejectsOversizeAndMalformedUnicode() {
        QVERIFY(setText("safe"));
        QVERIFY(!setText(std::string(PLANK_CLIPBOARD_MAX_TEXT_SIZE+1, 'x')));
        QVERIFY(!setText(std::string("a\0b", 3)));
        QVERIFY(!setText(std::string("\xED\xA0\x80",3)));
        QCOMPARE(*ClipboardPlatform::readText(), std::string("safe"));
    }
};
int main(int argc, char** argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--hold-clipboard") == 0) {
        HWND window = CreateWindowExW(0, L"STATIC", L"Clipboard test lock", 0,
            0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!window || !OpenClipboard(window)) return 2;
        std::puts("LOCKED"); std::fflush(stdout);
        std::getchar();
        CloseClipboard(); DestroyWindow(window);
        return 0;
    }
    QGuiApplication application(argc, argv);
    TestWindowsClipboardSync test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_windowsclipboardsync.moc"
