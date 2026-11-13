#include <QtTest>
#include <QGuiApplication>
#include <QSignalSpy>
#include "../../app/streaming/qt/sessioninput.h"
#include "../../app/streaming/input/linuxrawwacom.h"
#include "../../app/streaming/input/linuxwacom.h"
#include <mutex>
#include <atomic>
#include <thread>
#include <utility>
#include <plank_transport_pointer.h>
#ifdef Q_OS_MACOS
#include "../../app/streaming/input/macrawwacom.h"
#include "../../app/streaming/mackeyboardcapture.h"
#include "../../app/streaming/macquitshortcut.h"
#endif

// Link-time test doubles only. The production runtime uses the unchanged
// readers; these tests require no physical device or transport connection.
namespace {
struct Wire { int kind, a, b, c, d; };
QList<Wire> wire;
int positionResult = 0;
QList<PlankPointerPacket> pointerRecords;
std::mutex tabletMutex;
QStringList tabletCalls;
std::function<void()> tabletActivityHandler;
std::function<bool()> tabletBeforePointer;
std::function<void()> tabletAfterPointer;
bool tabletUsedGui = false;
bool normalized = false;
void tabletCall(const QString& call)
{
    std::lock_guard<std::mutex> lock(tabletMutex);
    tabletUsedGui |= QThread::currentThread() == qGuiApp->thread();
    tabletCalls.append(call);
}
bool saw(const QString& call)
{
    std::lock_guard<std::mutex> lock(tabletMutex);
    return tabletCalls.contains(call);
}
void registerTabletActivity(std::function<void()> activity)
{
    std::lock_guard<std::mutex> lock(tabletMutex);
    tabletActivityHandler = std::move(activity);
}
struct Fixture {
    QtSessionInput input;
    Fixture() { input.setStreamSize({5120, 2160}); input.setEnabled(true); input.setFocused(true); }
    ~Fixture() { input.releaseAll(); std::thread([this] { input.shutdown(); }).join(); }
    void start(unsigned flags) { std::thread([&] { input.startTablets(flags); }).join(); }
    int finishes = 0; bool reanchored = false; QPointF anchor;
    bool beginSucceeds = true;
    void capture() {
        input.setPointerCaptureSupported(true);
        input.setPointerCaptureHooks([this] { return beginSucceeds; }, [this](QPointF point, bool reanchor) {
            ++finishes; reanchored = reanchor; anchor = point;
        });
        input.setImmersiveMouse(true, {100, 200});
    }
    void ack() {
        auto packet = pointerRecords.last(); packet.operation = PLANK_POINTER_END_ACK;
        packet.x = 42.5f; packet.y = 83.25f;
        unsigned char bytes[PLANK_POINTER_PACKET_SIZE]; plank_pointer_encode(bytes, &packet);
        input.handlePointerCapture(bytes, sizeof(bytes));
        QCoreApplication::processEvents();
    }
};
}
extern "C" int LiSendPlankPointerCapture(const unsigned char* bytes, unsigned int size)
{
    PlankPointerPacket packet;
    if (plank_pointer_decode(bytes, size, &packet)) return -1;
    pointerRecords.append(packet); wire.append({5, packet.operation, 0, 0, 0}); return 0;
}
extern "C" int LiSendMousePositionEvent(short x, short y, short w, short h)
{ wire.append({0, x, y, w, h}); return positionResult; }
extern "C" int LiSendMouseButtonEvent(char action, int button)
{ wire.append({1, action, button, 0, 0}); return 0; }
extern "C" int LiSendKeyboardEvent2(short key, char action, char modifiers, char flags)
{ wire.append({2, static_cast<unsigned short>(key) & 0xff, action, modifiers, flags}); return 0; }
extern "C" int LiSendHighResScrollEvent(short amount) { wire.append({3, amount, 0, 0, 0}); return 0; }
extern "C" int LiSendHighResHScrollEvent(short amount) { wire.append({4, amount, 0, 0, 0}); return 0; }
extern "C" int LiSendUtf8TextEvent(const char*, unsigned int) { return 0; }
#ifdef Q_OS_MACOS
extern "C" int LiSendPenEvent(uint8_t, uint8_t, uint8_t, float, float, float, float, float, uint16_t, uint8_t) { return 0; }
class MacRawWacomInput::Impl {};
MacRawWacomInput::MacRawWacomInput(std::function<void()> activity, std::function<bool()> before, std::function<void()> after) {
    registerTabletActivity(std::move(activity));
    tabletBeforePointer = std::move(before); tabletAfterPointer = std::move(after);
}
MacRawWacomInput::~MacRawWacomInput() { registerTabletActivity({}); tabletBeforePointer = {}; tabletAfterPointer = {}; }
void MacRawWacomInput::setActive(bool) {}
void MacRawWacomInput::beginReconnect() {}
void MacRawWacomInput::finishReconnect() {}
void MacRawWacomInput::handleControl(const unsigned char*, unsigned int) {}
struct MacKeyboardCapture::State {};
MacKeyboardCapture::MacKeyboardCapture(std::function<bool()>, std::function<void()>) {}
MacKeyboardCapture::~MacKeyboardCapture() = default;
void MacKeyboardCapture::refresh() {}
void MacKeyboardCapture::setQueueWake(std::function<bool()>) {}
void MacKeyboardCapture::dispatchPending(const std::function<void(SDL_KeyboardEvent*)>&) {}
bool MacKeyboardCapture::suppressSdlKeyEvent() const { return false; }
struct MacQuitShortcut::State {};
MacQuitShortcut::MacQuitShortcut(std::function<bool()>) {}
MacQuitShortcut::~MacQuitShortcut() = default;
void MacQuitShortcut::refresh() {}
#endif

PlankWacomTransportDecision plankWacomTransportForConnectedDevice()
{ return {normalized ? PlankWacomTransport::NormalizedPen : PlankWacomTransport::ExactRawHid, 0, 0}; }
LinuxRawWacomInput::LinuxRawWacomInput(std::function<void()> activity)
{ registerTabletActivity(std::move(activity)); tabletCall("raw-create"); }
LinuxRawWacomInput::~LinuxRawWacomInput() { registerTabletActivity({}); tabletCall("raw-destroy"); }
void LinuxRawWacomInput::setActive(bool active) { tabletCall(active ? "raw-active" : "raw-inactive"); }
void LinuxRawWacomInput::beginReconnect() { tabletCall("raw-suspend"); }
void LinuxRawWacomInput::finishReconnect() { tabletCall("raw-resume"); }
void LinuxRawWacomInput::handleControl(const unsigned char*, unsigned int) {}
LinuxWacomInput::LinuxWacomInput(std::function<void()> activity)
{ registerTabletActivity(std::move(activity)); tabletCall("pen-create"); }
LinuxWacomInput::~LinuxWacomInput() { registerTabletActivity({}); tabletCall("pen-destroy"); }
void LinuxWacomInput::setActive(bool active) { tabletCall(active ? "pen-active" : "pen-inactive"); }
void LinuxWacomInput::beginReconnect() { tabletCall("pen-suspend"); }
void LinuxWacomInput::finishReconnect() { tabletCall("pen-resume"); }

class QtInputRuntimeTest : public QObject
{
    Q_OBJECT
private slots:
    void init()
    {
        wire.clear();
        pointerRecords.clear();
        positionResult = 0;
        std::lock_guard<std::mutex> lock(tabletMutex);
        tabletCalls.clear(); tabletUsedGui = false; normalized = false;
        tabletActivityHandler = {};
        qunsetenv("PLANK_EXTERNAL_WACOM_BRIDGE");
    }
    void pointerDragHasOrderedDeltasAndAuthoritativeEnd()
    {
        Fixture f; f.capture();
        f.input.mouseButton(Qt::MiddleButton, true, {5000, 2100});
        QCOMPARE(wire.size(), 2); QCOMPARE(wire[0].kind, 5); QCOMPARE(wire[1].kind, 1);
        QCOMPARE(pointerRecords[0].operation, uint8_t(PLANK_POINTER_BEGIN));
        f.input.mouseDelta({.25, -.75});
        f.input.mouseMotion({0, 0}); // The native absolute duplicate is never sent.
        QCOMPARE(pointerRecords.last().x, .25f); QCOMPARE(pointerRecords.last().y, -.75f);
        f.input.mouseButton(Qt::MiddleButton, false, {});
        QVERIFY(f.input.immersiveMouse());
        QCOMPARE(pointerRecords.last().operation, uint8_t(PLANK_POINTER_MOVE));
        f.input.mouseDelta({1, 0}); // Keyboard-started modal tools keep receiving deltas too.
        f.input.setImmersiveMouse(false);
        QCOMPARE(wire[wire.size()-3].kind, 1);
        QCOMPARE(pointerRecords.last().operation, uint8_t(PLANK_POINTER_END));
        QCOMPARE(f.finishes, 0);
        f.ack(); QCOMPARE(f.finishes, 1); QVERIFY(f.reanchored);
        QCOMPARE(f.anchor, QPointF(42.5, 83.25));
        f.input.mouseMotion({50, 90}); QCOMPARE(wire.last().kind, 0);
    }
    void pointerEndDefersInputWithoutLosingNextClick()
    {
        Fixture f; f.capture();
        f.input.mouseButton(Qt::LeftButton, true, {100, 200});
        f.input.mouseButton(Qt::LeftButton, false, {});
        f.input.setImmersiveMouse(false);
        const auto count = wire.size();
        f.input.mouseDelta({2.5, 1.25});
        f.input.mouseButton(Qt::RightButton, true, {});
        f.input.mouseDelta({.5, .5});
        f.input.mouseButton(Qt::RightButton, false, {});
        QCOMPARE(wire.size(), count);
        f.ack();
        QCOMPARE(pointerRecords.size(), 2);
        QCOMPARE(f.finishes, 1);
        QCOMPARE(f.anchor, QPointF(45.5, 85.0));
        int presses = 0, releases = 0;
        for (const auto& event : wire) if (event.kind == 1) {
            presses += event.a == BUTTON_ACTION_PRESS; releases += event.a == BUTTON_ACTION_RELEASE;
        }
        QCOMPARE(presses, 2); QCOMPARE(releases, 2);
    }
    void pointerFocusCancellationReleasesAndIgnoresLateAck()
    {
        Fixture f; f.capture();
        f.input.mouseButton(Qt::LeftButton, true, {100, 200});
        f.input.setFocused(false);
        QCOMPARE(wire[wire.size()-2].kind, 1);
        QCOMPARE(pointerRecords.last().operation, uint8_t(PLANK_POINTER_CANCEL));
        QCOMPARE(f.finishes, 1); QVERIFY(!f.reanchored);
        f.ack(); QCOMPARE(f.finishes, 1);
    }
    void pointerMultipleButtonsShareOneCaptureAndWheelRemainsOrdered()
    {
        Fixture f; f.capture();
        f.input.mouseButton(Qt::LeftButton, true, {100, 200});
        f.input.mouseButton(Qt::MiddleButton, true, {});
        f.input.mouseButton(Qt::LeftButton, false, {});
        QCOMPARE(pointerRecords.size(), 1);
        f.input.wheel({}, {0,120}, {}, false); QCOMPARE(wire.last().kind, 3);
        f.input.mouseButton(Qt::MiddleButton, false, {});
        f.input.setImmersiveMouse(false);
        f.ack(); QCOMPARE(f.finishes, 1);
    }
    void immersiveModeCanRestartWhileEndAckIsDelayed()
    {
        Fixture f; f.capture();
        f.input.setImmersiveMouse(false);
        f.input.setImmersiveMouse(true);
        f.input.mouseDelta({.5, .25});
        f.ack();
        QVERIFY(f.input.immersiveMouse()); QCOMPARE(f.finishes, 0);
        QCOMPARE(pointerRecords.last().operation, uint8_t(PLANK_POINTER_MOVE));
        QCOMPARE(pointerRecords.last().epoch, uint64_t(2));
        f.input.setImmersiveMouse(false); f.ack(); QCOMPARE(f.finishes, 1);
    }
    void immersiveEndKeepsModifiersClicksAndWheelInOrder()
    {
        Fixture f; f.capture(); f.input.setImmersiveMouse(false);
#ifdef Q_OS_MACOS
        constexpr quint32 shiftScan = 56;
#else
        constexpr quint32 shiftScan = KEY_LEFTSHIFT + 8;
#endif
        const auto old = wire.size();
        f.input.key(shiftScan, Qt::Key_Shift, true, false, Qt::ShiftModifier);
        f.input.mouseDelta({2, 0});
        f.input.mouseButton(Qt::LeftButton, true, {});
        f.input.wheel({}, {0, 120}, {}, false);
        f.input.mouseButton(Qt::LeftButton, false, {});
        f.input.key(shiftScan, Qt::Key_Shift, false, false, {});
        QCOMPARE(wire.size(), old);
        f.ack();
        QList<int> kinds;
        for (int i=old; i<wire.size(); ++i) kinds.append(wire[i].kind);
        QCOMPARE(kinds, QList<int>({2, 0, 0, 1, 3, 1, 2}));
        QCOMPARE(wire[old].b, int(KEY_ACTION_DOWN));
        QCOMPARE(wire.last().b, int(KEY_ACTION_UP));
    }
    void immersiveTabletCancellationPreservesHeldKeyboardModifiers()
    {
        Fixture f; f.capture();
#ifdef Q_OS_MACOS
        constexpr quint32 shiftScan = 56;
#else
        constexpr quint32 shiftScan = KEY_LEFTSHIFT + 8;
#endif
        f.input.key(shiftScan, Qt::Key_Shift, true, false, Qt::ShiftModifier);
        f.input.mouseButton(Qt::LeftButton, true, {});
        f.input.cancelPointerCapture();
        QCOMPARE(pointerRecords.last().operation, uint8_t(PLANK_POINTER_CANCEL));
        int ups = 0;
        for (const auto& event : wire) if (event.kind==2 && event.b==KEY_ACTION_UP) ++ups;
        QCOMPARE(ups, 0);
        f.input.key(shiftScan, Qt::Key_Shift, false, false, {});
        QCOMPARE(wire.last().kind, 2); QCOMPARE(wire.last().b, int(KEY_ACTION_UP));
    }
    void immersiveActivationDuringNormalDragDoesNotStartOrCancelThatDrag()
    {
        Fixture f;
        f.input.mouseButton(Qt::LeftButton, true, {100,200});
        f.capture();
        QVERIFY(!f.input.immersiveMouse()); QVERIFY(pointerRecords.isEmpty());
        QCOMPARE(f.finishes,0);
        f.input.mouseButton(Qt::LeftButton,false,{});
        QCOMPARE(wire.last().kind,1); QCOMPARE(wire.last().a,int(BUTTON_ACTION_RELEASE));
        f.input.setImmersiveMouse(true,{100,200});
        QVERIFY(f.input.immersiveMouse());
        f.input.setImmersiveMouse(false); f.ack();
    }
    void immersiveFailedRestartDiscardsDeferredInputAndReanchorsOnlyOnce()
    {
        Fixture f; f.capture();
        f.input.setImmersiveMouse(false);
        f.input.setImmersiveMouse(true);
        f.input.mouseDelta({20, 30});
        f.input.mouseButton(Qt::LeftButton, true, {});
        f.input.wheel({}, {0,120}, {}, false);
        f.beginSucceeds = false;
        const auto before = wire.size();
        QSignalSpy failure(&f.input, &QtSessionInput::pointerCaptureFailed);
        f.ack();
        QCOMPARE(failure.count(), 1);
        QCOMPARE(wire.size(), before);
        QCOMPARE(f.finishes, 1); QVERIFY(!f.reanchored);
        QVERIFY(!f.input.immersiveMouse());
    }
#ifdef Q_OS_MACOS
    void immersiveRawTabletHandoffOrdersCancelBeforeReportAndExcludesNewBegin()
    {
        Fixture f; f.start(LI_FF_RAW_HID_TABLET | LI_FF_RAW_HID_FOCUS_SUSPEND);
        QVERIFY(tabletBeforePointer); QVERIFY(tabletAfterPointer);
        f.capture();
        std::atomic<bool> done{false};
        bool accepted = false;
        std::thread reader([&] {
            accepted = tabletBeforePointer();
            if (accepted) { wire.append({6,0,0,0,0}); tabletAfterPointer(); }
            done.store(true);
        });
        while (!done.load()) QCoreApplication::processEvents();
        reader.join();
        QVERIFY(accepted); QVERIFY(!f.input.immersiveMouse());
        QCOMPARE(wire[wire.size()-2].kind, 5);
        QCOMPARE(wire[wire.size()-2].a, int(PLANK_POINTER_CANCEL));
        QCOMPARE(wire.last().kind, 6);
    }
    void immersiveBeginDoesNotSplitRawTabletReportEnqueue()
    {
        Fixture f; f.start(LI_FF_RAW_HID_TABLET | LI_FF_RAW_HID_FOCUS_SUSPEND);
        std::atomic<bool> locked{false}, release{false}; bool accepted = false;
        std::thread reader([&] {
            accepted = tabletBeforePointer(); locked.store(true);
            while (!release.load()) std::this_thread::yield();
            if (accepted) tabletAfterPointer();
        });
        while (!locked.load()) std::this_thread::yield();
        f.capture();
        const bool enabled = f.input.immersiveMouse();
        release.store(true); reader.join();
        QVERIFY(accepted); QVERIFY(!enabled); QVERIFY(pointerRecords.isEmpty());
        QCOMPARE(f.finishes, 0);
    }
#endif
    void mouseOrderingAndFocusRelease()
    {
        Fixture f;
        f.input.mouseButton(Qt::LeftButton, true, {5000, 2100});
        QCOMPARE(wire.size(), 2); QCOMPARE(wire[0].kind, 0);
        QCOMPARE(wire[0].a, 5000); QCOMPARE(wire[0].c, 5120);
        QCOMPARE(wire[1].kind, 1); QCOMPARE(wire[1].a, int(BUTTON_ACTION_PRESS));
        f.input.setFocused(false);
        QCOMPARE(wire.size(), 3); QCOMPARE(wire.last().a, int(BUTTON_ACTION_RELEASE));
        f.input.mouseButton(Qt::LeftButton, false, {0, 0});
        QCOMPARE(wire.size(), 3);
        f.input.setEnabled(false);
        f.input.mouseButton(Qt::LeftButton, true, {0, 0});
        QCOMPARE(wire.size(), 3);
    }
    void physicalModifiersRepeatAndRelease()
    {
#ifdef Q_OS_MACOS
        QSKIP("Linux physical scan-code fixture; Mac capture has its own suite");
#else
        Fixture f;
        f.input.key(KEY_LEFTSHIFT + 8, Qt::Key_Shift, true, false, Qt::ShiftModifier);
        f.input.key(KEY_A + 8, Qt::Key_A, true, false, Qt::ShiftModifier);
        f.input.key(KEY_A + 8, Qt::Key_A, true, true, Qt::ShiftModifier);
        QCOMPARE(wire.size(), 2); QCOMPARE(wire[0].a, 0xa0);
        QCOMPARE(wire[1].a, 0x41); QCOMPARE(wire[1].c, int(MODIFIER_SHIFT));
        f.input.setEnabled(false);
        QCOMPARE(wire.size(), 4);
        QCOMPARE(wire[2].b, int(KEY_ACTION_UP)); QCOMPARE(wire[3].b, int(KEY_ACTION_UP));
        f.input.key(KEY_A + 8, Qt::Key_A, false, false, {});
        QCOMPARE(wire.size(), 4);
#endif
    }
    void shortcutsStayLocalAndWheelUnitsStayExact()
    {
        Fixture f;
#ifndef Q_OS_MACOS
        QSignalSpy shortcut(&f.input, &QtSessionInput::shortcutRequested);
        f.input.key(KEY_Q + 8, Qt::Key_Q, true, false, Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier);
        f.input.key(KEY_Q + 8, Qt::Key_Q, false, false, {});
        QCOMPARE(shortcut.size(), 1); QCOMPARE(shortcut[0][0].toInt(), int(Qt::Key_Q));
        QVERIFY(wire.isEmpty());
#endif
        f.input.mouseMotion({100, 200});
        wire.clear();
        f.input.wheel({100, 200}, {-30, 120}, {}, false);
        QCOMPARE(wire.size(), 2); QCOMPARE(wire[0].kind, 3); QCOMPARE(wire[0].a, 120);
        QCOMPARE(wire[1].kind, 4); QCOMPARE(wire[1].a, -30);
    }
    void stationaryWheelDoesNotInjectMotion()
    {
        Fixture f;
        f.input.mouseMotion({100.1, 200.1});
        wire.clear();
        for (const int delta : {120, 120, -120, -30, -30, -30, -30, 240})
            f.input.wheel({100.2, 200.2}, {0, delta}, {}, false);
        QCOMPARE(wire.size(), 8);
        for (const auto& event : wire) QCOMPARE(event.kind, 3);
        QCOMPARE(wire[3].a, -30); QCOMPARE(wire.last().a, 240);

        // Zero-delta phase events must not disturb the Host's pointer source.
        wire.clear();
        f.input.wheel({300, 400}, {}, {}, false);
        QVERIFY(wire.isEmpty());
    }
    void wheelPositionsOnlyWhenNeeded()
    {
        Fixture f;
        f.input.wheel({100, 200}, {0, 120}, {}, false);
        QCOMPARE(wire.size(), 2); QCOMPARE(wire[0].kind, 0); QCOMPARE(wire[1].kind, 3);
        wire.clear();
        f.input.wheel({100, 200}, {0, 120}, {}, false);
        QCOMPARE(wire.size(), 1); QCOMPARE(wire[0].kind, 3);

        // Another viewport may deliver a wheel without an intervening motion.
        wire.clear();
        f.input.wheel({5000, 2000}, {120, 0}, {}, false);
        QCOMPARE(wire.size(), 2); QCOMPARE(wire[0].kind, 0); QCOMPARE(wire[0].a, 5000);
        QCOMPARE(wire[0].b, 2000); QCOMPARE(wire[1].kind, 4);

        // Button and real motion behavior stays unchanged; their last position
        // also satisfies a subsequent wheel's positioning requirement.
        wire.clear();
        f.input.mouseButton(Qt::LeftButton, true, {5000, 2000});
        f.input.mouseButton(Qt::LeftButton, false, {5000, 2000});
        QCOMPARE(wire.size(), 3); QCOMPARE(wire[0].kind, 0);
        f.input.mouseMotion({5000, 2000});
        QCOMPARE(wire.size(), 4); QCOMPARE(wire.last().kind, 0);
        f.input.wheel({5000, 2000}, {0, -120}, {}, false);
        QCOMPARE(wire.size(), 5); QCOMPARE(wire.last().kind, 3);
    }
    void wheelRepositionsAfterInputAndStreamChanges()
    {
        Fixture f;
        f.input.mouseMotion({100, 200});
        wire.clear();
        f.input.setFocused(false); f.input.setFocused(true);
        f.input.wheel({100, 200}, {0, 120}, {}, false);
        QCOMPARE(wire.size(), 2); QCOMPARE(wire[0].kind, 0);

        wire.clear();
        f.input.setEnabled(false);
        f.input.wheel({100, 200}, {0, 120}, {}, false);
        QVERIFY(wire.isEmpty());
        f.input.setEnabled(true);
        f.input.wheel({100, 200}, {0, 120}, {}, false);
        QCOMPARE(wire.size(), 2); QCOMPARE(wire[0].kind, 0);

        wire.clear();
        f.input.setStreamSize({3840, 2160});
        f.input.wheel({100, 200}, {0, 120}, {}, false);
        QCOMPARE(wire.size(), 2); QCOMPARE(wire[0].kind, 0); QCOMPARE(wire[0].c, 3840);

        wire.clear();
        QSignalSpy shape(&f.input, &QtSessionInput::cursorShape);
        std::thread([&] { f.input.resetCursorEpoch(); }).join();
        QTRY_COMPARE(shape.size(), 1);
        f.input.wheel({100, 200}, {0, 120}, {}, false);
        QCOMPARE(wire.size(), 2); QCOMPARE(wire[0].kind, 0);

        wire.clear();
        f.input.setStreamSize({0, 0});
        f.input.wheel({100, 200}, {0, 120}, {}, false);
        QVERIFY(wire.isEmpty());
        f.input.setStreamSize({3840, 2160});
        f.input.wheel({100, 200}, {0, 120}, {}, false);
        QCOMPARE(wire.size(), 2); QCOMPARE(wire[0].kind, 0);
    }
    void wheelRetriesFailedPosition()
    {
        Fixture f;
        f.input.mouseMotion({100, 200});
        positionResult = -1;
        f.input.mouseMotion({300, 400});
        wire.clear();
        f.input.wheel({300, 400}, {0, 120}, {}, false);
        QCOMPARE(wire.size(), 1); QCOMPARE(wire[0].kind, 0);
        wire.clear();
        positionResult = 0;
        f.input.wheel({300, 400}, {0, 120}, {}, false);
        QCOMPARE(wire.size(), 2); QCOMPARE(wire[0].kind, 0); QCOMPARE(wire[1].kind, 3);
        wire.clear();
        f.input.wheel({300, 400}, {0, 120}, {}, false);
        QCOMPARE(wire.size(), 1); QCOMPARE(wire[0].kind, 3);
    }
    void wheelRepositionsAfterTablet_data()
    {
        QTest::addColumn<bool>("pen");
        QTest::addColumn<bool>("pending");
        QTest::newRow("raw-active") << false << false;
        QTest::newRow("raw-pending") << false << true;
        QTest::newRow("normalized-active") << true << false;
        QTest::newRow("normalized-pending") << true << true;
    }
    void wheelRepositionsAfterTablet()
    {
        QFETCH(bool, pen);
        QFETCH(bool, pending);
        normalized = pen;
        Fixture f;
        f.start(LI_FF_RAW_HID_TABLET | LI_FF_RAW_HID_FOCUS_SUSPEND | LI_FF_PEN_TOUCH_EVENTS);
        QTRY_VERIFY(saw(pen ? "pen-active" : "raw-active"));
        std::function<void()> activity;
        { std::lock_guard<std::mutex> lock(tabletMutex); activity = tabletActivityHandler; }
        QVERIFY(activity);
        f.input.mouseMotion({100, 200});
        wire.clear();
        QSignalSpy active(&f.input, &QtSessionInput::tabletActive);
        activity();
        if (!pending) QTRY_COMPARE(active.size(), 1);
        f.input.wheel({100, 200}, {0, 120}, {}, false);
        QCOMPARE(wire.size(), 2); QCOMPARE(wire[0].kind, 0); QCOMPARE(wire[1].kind, 3);
        if (!pending) { QCOMPARE(active.size(), 2); QVERIFY(!active.last()[0].toBool()); }
        wire.clear();
        QCoreApplication::processEvents();
        f.input.wheel({100, 200}, {0, 120}, {}, false);
        QCOMPARE(wire.size(), 1); QCOMPARE(wire[0].kind, 3);
    }
    void tabletLifecycle_data()
    {
        QTest::addColumn<bool>("pen");
        QTest::newRow("descriptor-driven-raw") << false;
        QTest::newRow("normalized-first-generation") << true;
    }
    void tabletLifecycle()
    {
        QFETCH(bool, pen);
        normalized = pen;
        const auto prefix = pen ? QStringLiteral("pen-") : QStringLiteral("raw-");
        {
            Fixture f;
            f.start(LI_FF_RAW_HID_TABLET | LI_FF_RAW_HID_FOCUS_SUSPEND | LI_FF_PEN_TOUCH_EVENTS);
            QTRY_VERIFY(saw(prefix + "active"));
            f.input.setFocused(false);
            QTRY_VERIFY(saw(prefix + "inactive"));
            std::thread([&] { f.input.beginReconnect(); f.input.resetCursorEpoch(); f.input.finishReconnect(); }).join();
            QVERIFY(saw(prefix + "suspend")); QVERIFY(saw(prefix + "resume"));
            f.input.setFocused(true);
        }
        QVERIFY(saw(prefix + "destroy"));
        std::lock_guard<std::mutex> lock(tabletMutex);
        QVERIFY(!tabletUsedGui);
        QCOMPARE(tabletCalls.count(prefix + "create"), 1);
        QCOMPARE(tabletCalls.count(prefix + "destroy"), 1);
    }
    void externalTabletBridgeDoesNotOpenASecondReader()
    {
        qputenv("PLANK_EXTERNAL_WACOM_BRIDGE", "1");
        { Fixture f; f.start(LI_FF_RAW_HID_TABLET | LI_FF_RAW_HID_FOCUS_SUSPEND | LI_FF_PEN_TOUCH_EVENTS); }
        std::lock_guard<std::mutex> lock(tabletMutex);
        QVERIFY(tabletCalls.isEmpty());
    }
};
QTEST_MAIN(QtInputRuntimeTest)
#include "test_qtinputruntime.moc"
