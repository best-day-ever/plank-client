#include <QtTest>

#include "macpen.h"
#include "pentiltencoding.h"

#include <cmath>
#include <limits>
#include <vector>

namespace {
constexpr std::uint32_t kPenA = 1;
constexpr std::uint32_t kPenB = 2;
} // namespace

class TestMacPen : public QObject
{
    Q_OBJECT

private slots:
    void noPacketsBeforeActive();
    void proximityInSendsNothingUntilPositionKnown();
    void motionWhileHoveringSendsHover();
    void downThenUpSendsDownAndUp();
    void axisPressureUpdatesWhileDown();
    void tiltAxesMatchSharedEncoder();
    void unknownTiltUntilBothAxesReported();
    void eraserFlagSelectsEraserTool();
    void buttonFlagsMapToWireBits();
    void higherButtonBitsAreIgnored();
    void proximityOutSendsHoverLeaveAndResets();
    void deactivateCancelsInProximityPen();
    void deactivateWithNoPenSendsNothing();
    void reactivatingRequiresNewProximity();
    void secondPenSwapsOutTheFirst();
    void outOfRangeAxisValuesAreClamped();
};

void TestMacPen::noPacketsBeforeActive()
{
    std::vector<PlankMacPenPacket> sent;
    PlankMacPenInput input([&](const PlankMacPenPacket& p) { sent.push_back(p); });

    input.handleProximityIn(kPenA);
    input.handlePosition(kPenA, 0.25f, 0.5f, 0);
    input.handleTouch(kPenA, true, 0.25f, 0.5f, 0);

    QCOMPARE(sent.size(), std::size_t(0));
}

void TestMacPen::proximityInSendsNothingUntilPositionKnown()
{
    std::vector<PlankMacPenPacket> sent;
    PlankMacPenInput input([&](const PlankMacPenPacket& p) { sent.push_back(p); });
    input.setActive(true);

    input.handleProximityIn(kPenA);
    QCOMPARE(sent.size(), std::size_t(0));

    input.handlePosition(kPenA, 0.1f, 0.2f, 0);
    QCOMPARE(sent.size(), std::size_t(1));
    QCOMPARE(sent[0].eventType, static_cast<std::uint8_t>(LI_TOUCH_EVENT_HOVER));
}

void TestMacPen::motionWhileHoveringSendsHover()
{
    std::vector<PlankMacPenPacket> sent;
    PlankMacPenInput input([&](const PlankMacPenPacket& p) { sent.push_back(p); });
    input.setActive(true);
    input.handleProximityIn(kPenA);

    input.handlePosition(kPenA, 0.3f, 0.7f, 0);

    QCOMPARE(sent.size(), std::size_t(1));
    const PlankMacPenPacket& p = sent.back();
    QCOMPARE(p.eventType, static_cast<std::uint8_t>(LI_TOUCH_EVENT_HOVER));
    QCOMPARE(p.toolType, static_cast<std::uint8_t>(LI_TOOL_TYPE_PEN));
    QCOMPARE(p.x, 0.3f);
    QCOMPARE(p.y, 0.7f);
}

void TestMacPen::downThenUpSendsDownAndUp()
{
    std::vector<PlankMacPenPacket> sent;
    PlankMacPenInput input([&](const PlankMacPenPacket& p) { sent.push_back(p); });
    input.setActive(true);
    input.handleProximityIn(kPenA);

    input.handleTouch(kPenA, true, 0.5f, 0.5f, 0);
    QCOMPARE(sent.back().eventType, static_cast<std::uint8_t>(LI_TOUCH_EVENT_DOWN));

    input.handleTouch(kPenA, false, 0.5f, 0.5f, 0);
    QCOMPARE(sent.back().eventType, static_cast<std::uint8_t>(LI_TOUCH_EVENT_UP));
}

void TestMacPen::axisPressureUpdatesWhileDown()
{
    std::vector<PlankMacPenPacket> sent;
    PlankMacPenInput input([&](const PlankMacPenPacket& p) { sent.push_back(p); });
    input.setActive(true);
    input.handleProximityIn(kPenA);
    input.handleTouch(kPenA, true, 0.5f, 0.5f, 0);

    input.handleAxis(kPenA, PlankMacPenAxis::Pressure, 0.75f);

    QCOMPARE(sent.back().eventType, static_cast<std::uint8_t>(LI_TOUCH_EVENT_MOVE));
    QVERIFY(std::fabs(double(sent.back().pressureOrDistance) - 0.75) < 1e-6);
}

void TestMacPen::tiltAxesMatchSharedEncoder()
{
    std::vector<PlankMacPenPacket> sent;
    PlankMacPenInput input([&](const PlankMacPenPacket& p) { sent.push_back(p); });
    input.setActive(true);
    input.handleProximityIn(kPenA);
    input.handlePosition(kPenA, 0.5f, 0.5f, 0);

    input.handleAxis(kPenA, PlankMacPenAxis::XTilt, 30.0f);
    input.handleAxis(kPenA, PlankMacPenAxis::YTilt, -20.0f);

    unsigned short expectedRotation = 0;
    unsigned char expectedTilt = 0;
    plankEncodePenTilt(30.0, -20.0, expectedRotation, expectedTilt);

    QCOMPARE(sent.back().tilt, expectedTilt);
    QCOMPARE(sent.back().rotation, expectedRotation);
}

void TestMacPen::unknownTiltUntilBothAxesReported()
{
    std::vector<PlankMacPenPacket> sent;
    PlankMacPenInput input([&](const PlankMacPenPacket& p) { sent.push_back(p); });
    input.setActive(true);
    input.handleProximityIn(kPenA);
    input.handlePosition(kPenA, 0.5f, 0.5f, 0);
    QCOMPARE(sent.back().tilt, static_cast<std::uint8_t>(LI_TILT_UNKNOWN));
    QCOMPARE(sent.back().rotation, static_cast<std::uint16_t>(LI_ROT_UNKNOWN));

    // Only one axis reported: still unknown, since a magnitude/direction
    // pair cannot be derived from a single independent angle.
    input.handleAxis(kPenA, PlankMacPenAxis::XTilt, 10.0f);
    QCOMPARE(sent.back().tilt, static_cast<std::uint8_t>(LI_TILT_UNKNOWN));
    QCOMPARE(sent.back().rotation, static_cast<std::uint16_t>(LI_ROT_UNKNOWN));

    input.handleAxis(kPenA, PlankMacPenAxis::YTilt, 0.0f);
    QVERIFY(sent.back().tilt != static_cast<std::uint8_t>(LI_TILT_UNKNOWN));
}

void TestMacPen::eraserFlagSelectsEraserTool()
{
    std::vector<PlankMacPenPacket> sent;
    PlankMacPenInput input([&](const PlankMacPenPacket& p) { sent.push_back(p); });
    input.setActive(true);
    input.handleProximityIn(kPenA);

    input.handlePosition(kPenA, 0.5f, 0.5f, 0);
    QCOMPARE(sent.back().toolType, static_cast<std::uint8_t>(LI_TOOL_TYPE_PEN));

    input.handlePosition(kPenA, 0.5f, 0.5f, PlankMacPenInputFlags::EraserTip);
    QCOMPARE(sent.back().toolType, static_cast<std::uint8_t>(LI_TOOL_TYPE_ERASER));
}

void TestMacPen::buttonFlagsMapToWireBits()
{
    std::vector<PlankMacPenPacket> sent;
    PlankMacPenInput input([&](const PlankMacPenPacket& p) { sent.push_back(p); });
    input.setActive(true);
    input.handleProximityIn(kPenA);

    input.handleButton(kPenA, PlankMacPenInputFlags::Button1);
    QCOMPARE(sent.back().eventType, static_cast<std::uint8_t>(LI_TOUCH_EVENT_BUTTON_ONLY));
    QCOMPARE(sent.back().buttons, static_cast<std::uint8_t>(LI_PEN_BUTTON_PRIMARY));

    input.handleButton(kPenA, PlankMacPenInputFlags::Button2);
    QCOMPARE(sent.back().buttons, static_cast<std::uint8_t>(LI_PEN_BUTTON_SECONDARY));

    input.handleButton(kPenA, PlankMacPenInputFlags::Button3);
    QCOMPARE(sent.back().buttons, static_cast<std::uint8_t>(LI_PEN_BUTTON_TERTIARY));

    input.handleButton(kPenA,
        PlankMacPenInputFlags::Button1 | PlankMacPenInputFlags::Button2);
    QCOMPARE(sent.back().buttons,
        static_cast<std::uint8_t>(LI_PEN_BUTTON_PRIMARY | LI_PEN_BUTTON_SECONDARY));
}

void TestMacPen::higherButtonBitsAreIgnored()
{
    std::vector<PlankMacPenPacket> sent;
    PlankMacPenInput input([&](const PlankMacPenPacket& p) { sent.push_back(p); });
    input.setActive(true);
    input.handleProximityIn(kPenA);

    // Bits 4/5 (SDL_PEN_INPUT_BUTTON_4/5) have no wire representation.
    input.handleButton(kPenA, 1u << 4);
    QCOMPARE(sent.back().buttons, static_cast<std::uint8_t>(0));
}

void TestMacPen::proximityOutSendsHoverLeaveAndResets()
{
    std::vector<PlankMacPenPacket> sent;
    PlankMacPenInput input([&](const PlankMacPenPacket& p) { sent.push_back(p); });
    input.setActive(true);
    input.handleProximityIn(kPenA);
    input.handlePosition(kPenA, 0.5f, 0.5f, 0);
    input.handleAxis(kPenA, PlankMacPenAxis::XTilt, 45.0f);
    input.handleAxis(kPenA, PlankMacPenAxis::YTilt, 0.0f);

    input.handleProximityOut(kPenA);
    QCOMPARE(sent.back().eventType, static_cast<std::uint8_t>(LI_TOUCH_EVENT_HOVER_LEAVE));

    // A fresh proximity/position for the same pen ID starts clean: tilt is
    // unknown again until both axes are reported anew.
    input.handleProximityIn(kPenA);
    input.handlePosition(kPenA, 0.1f, 0.1f, 0);
    QCOMPARE(sent.back().tilt, static_cast<std::uint8_t>(LI_TILT_UNKNOWN));
}

void TestMacPen::deactivateCancelsInProximityPen()
{
    std::vector<PlankMacPenPacket> sent;
    PlankMacPenInput input([&](const PlankMacPenPacket& p) { sent.push_back(p); });
    input.setActive(true);
    input.handleProximityIn(kPenA);
    input.handlePosition(kPenA, 0.5f, 0.5f, 0);

    input.setActive(false);

    QCOMPARE(sent.back().eventType, static_cast<std::uint8_t>(LI_TOUCH_EVENT_CANCEL_ALL));

    // Further events while inactive produce nothing.
    const std::size_t countAfterCancel = sent.size();
    input.handlePosition(kPenA, 0.2f, 0.2f, 0);
    QCOMPARE(sent.size(), countAfterCancel);
}

void TestMacPen::deactivateWithNoPenSendsNothing()
{
    std::vector<PlankMacPenPacket> sent;
    PlankMacPenInput input([&](const PlankMacPenPacket& p) { sent.push_back(p); });
    input.setActive(true);

    input.setActive(false);

    QCOMPARE(sent.size(), std::size_t(0));
}

void TestMacPen::reactivatingRequiresNewProximity()
{
    std::vector<PlankMacPenPacket> sent;
    PlankMacPenInput input([&](const PlankMacPenPacket& p) { sent.push_back(p); });
    input.setActive(true);
    input.handleProximityIn(kPenA);
    input.handlePosition(kPenA, 0.5f, 0.5f, 0);
    input.setActive(false);
    sent.clear();

    input.setActive(true);
    QCOMPARE(sent.size(), std::size_t(0));

    input.handlePosition(kPenA, 0.5f, 0.5f, 0);
    QCOMPARE(sent.size(), std::size_t(1));
}

void TestMacPen::secondPenSwapsOutTheFirst()
{
    std::vector<PlankMacPenPacket> sent;
    PlankMacPenInput input([&](const PlankMacPenPacket& p) { sent.push_back(p); });
    input.setActive(true);
    input.handleProximityIn(kPenA);
    input.handlePosition(kPenA, 0.1f, 0.1f, 0);
    sent.clear();

    input.handlePosition(kPenB, 0.9f, 0.9f, 0);

    QCOMPARE(sent.size(), std::size_t(2));
    QCOMPARE(sent[0].eventType, static_cast<std::uint8_t>(LI_TOUCH_EVENT_HOVER_LEAVE));
    QCOMPARE(sent[1].eventType, static_cast<std::uint8_t>(LI_TOUCH_EVENT_HOVER));
    QCOMPARE(sent[1].x, 0.9f);
}

void TestMacPen::outOfRangeAxisValuesAreClamped()
{
    std::vector<PlankMacPenPacket> sent;
    PlankMacPenInput input([&](const PlankMacPenPacket& p) { sent.push_back(p); });
    input.setActive(true);
    input.handleProximityIn(kPenA);
    input.handleTouch(kPenA, true, 2.0f, -1.0f, 0);

    QCOMPARE(double(sent.back().x), 1.0);
    QCOMPARE(double(sent.back().y), 0.0);

    input.handleAxis(kPenA, PlankMacPenAxis::Pressure,
                      std::numeric_limits<float>::quiet_NaN());
    QCOMPARE(double(sent.back().pressureOrDistance), 0.0);
}

QTEST_APPLESS_MAIN(TestMacPen)
#include "test_macpen.moc"
