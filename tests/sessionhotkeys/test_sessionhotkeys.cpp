#include "../../app/streaming/planksessionhotkeys.h"
#include <QtTest>

class TestSessionHotkeys : public QObject
{
    Q_OBJECT
    static SDL_KeyboardEvent key(SDL_Scancode scan, SDL_Keycode letter,
                                 bool down = true, SDL_Keymod mods =
                                 SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_SHIFT)
    {
        SDL_KeyboardEvent result {};
        result.scancode = scan;
        result.key = letter;
        result.down = down;
        result.mod = mods;
        return result;
    }
private slots:
    void reservedControls_data()
    {
        QTest::addColumn<int>("scan"); QTest::addColumn<quint32>("letter");
        QTest::addColumn<unsigned>("action");
        QTest::newRow("disconnect") << int(SDL_SCANCODE_Q) << quint32(SDLK_Q) << unsigned(PlankSessionHotkeys::Disconnect);
        QTest::newRow("toolbar") << int(SDL_SCANCODE_T) << quint32(SDLK_T) << unsigned(PlankSessionHotkeys::ToggleToolbar);
        QTest::newRow("non-latin-disconnect") << int(SDL_SCANCODE_Q) << quint32(0x0439) << unsigned(PlankSessionHotkeys::Disconnect);
        QTest::newRow("non-latin-toolbar") << int(SDL_SCANCODE_T) << quint32(0x0435) << unsigned(PlankSessionHotkeys::ToggleToolbar);
        QTest::newRow("logical-letter-wins") << int(SDL_SCANCODE_Q) << quint32(SDLK_T) << unsigned(PlankSessionHotkeys::ToggleToolbar);
    }
    void reservedControls()
    {
        QFETCH(int, scan); QFETCH(quint32, letter); QFETCH(unsigned, action);
        PlankSessionHotkeys controls;
        QVERIFY(controls.handle(key(SDL_Scancode(scan), letter)));
        QVERIFY(controls.pending());
        QCOMPARE(unsigned(controls.take()), action);
        QCOMPARE(controls.take(), PlankSessionHotkeys::None);
        // Releases are consumed even when modifiers were released first.
        QVERIFY(controls.handle(key(SDL_Scancode(scan), letter, false, SDL_KMOD_NONE)));
        QVERIFY(!controls.handle(key(SDL_Scancode(scan), letter, false, SDL_KMOD_NONE)));
    }
    void ordinaryAndIncompleteChordsPassThrough()
    {
        PlankSessionHotkeys controls;
        for (auto mods : {SDL_Keymod(0), SDL_Keymod(SDL_KMOD_CTRL | SDL_KMOD_ALT),
                          SDL_Keymod(SDL_KMOD_CTRL | SDL_KMOD_SHIFT),
                          SDL_Keymod(SDL_KMOD_ALT | SDL_KMOD_SHIFT)}) {
            QVERIFY(!controls.handle(key(SDL_SCANCODE_Q, SDLK_Q, true, mods)));
            QVERIFY(!controls.handle(key(SDL_SCANCODE_T, SDLK_T, true, mods)));
        }
        QVERIFY(!controls.handle(key(SDL_SCANCODE_A, SDLK_A)));
        QVERIFY(!controls.handle(key(SDL_SCANCODE_Z, SDLK_Z)));
        QCOMPARE(controls.take(), PlankSessionHotkeys::None);
    }
    void eitherSideAndMixedModifierKeysWork()
    {
        for (auto mods : {SDL_Keymod(SDL_KMOD_LCTRL | SDL_KMOD_LALT | SDL_KMOD_LSHIFT),
                          SDL_Keymod(SDL_KMOD_RCTRL | SDL_KMOD_RALT | SDL_KMOD_RSHIFT),
                          SDL_Keymod(SDL_KMOD_RCTRL | SDL_KMOD_LALT | SDL_KMOD_RSHIFT)}) {
            PlankSessionHotkeys controls;
            QVERIFY(controls.handle(key(SDL_SCANCODE_Q, SDLK_Q, true, mods)));
            QCOMPARE(controls.take(), PlankSessionHotkeys::Disconnect);
            QVERIFY(controls.handle(key(SDL_SCANCODE_T, SDLK_T, true, mods)));
            QCOMPARE(controls.take(), PlankSessionHotkeys::ToggleToolbar);
        }
    }
    void repeatsNeverToggleOrRetrigger()
    {
        PlankSessionHotkeys controls;
        auto event = key(SDL_SCANCODE_T, SDLK_T);
        QVERIFY(controls.handle(event));
        event.repeat = true;
        for (int i = 0; i < 1000; ++i) QVERIFY(controls.handle(event));
        QCOMPARE(controls.take(), PlankSessionHotkeys::ToggleToolbar);
        QVERIFY(controls.handle(event));
        QCOMPARE(controls.take(), PlankSessionHotkeys::None);
    }
    void disconnectWinsOverToolbarInEitherOrder()
    {
        for (bool disconnectFirst : {true, false}) {
            PlankSessionHotkeys controls;
            auto q = key(SDL_SCANCODE_Q, SDLK_Q), t = key(SDL_SCANCODE_T, SDLK_T);
            QVERIFY(controls.handle(disconnectFirst ? q : t));
            QVERIFY(controls.handle(disconnectFirst ? t : q));
            QCOMPARE(controls.take(), PlankSessionHotkeys::Disconnect);
            QCOMPARE(controls.take(), PlankSessionHotkeys::None);
        }
    }
    void twoToolbarPressesCancel()
    {
        PlankSessionHotkeys controls;
        QVERIFY(controls.handle(key(SDL_SCANCODE_T, SDLK_T)));
        QVERIFY(controls.handle(key(SDL_SCANCODE_T, SDLK_T, false)));
        QVERIFY(controls.handle(key(SDL_SCANCODE_T, SDLK_T)));
        QCOMPARE(controls.take(), PlankSessionHotkeys::None);
    }
    void focusLossClearsConsumedKeysButKeepsRequestedExit()
    {
        PlankSessionHotkeys controls;
        QVERIFY(controls.handle(key(SDL_SCANCODE_Q, SDLK_Q)));
        controls.releaseKeys();
        QVERIFY(!controls.handle(key(SDL_SCANCODE_Q, SDLK_Q, false)));
        QCOMPARE(controls.take(), PlankSessionHotkeys::Disconnect);
    }
};
QTEST_GUILESS_MAIN(TestSessionHotkeys)
#include "test_sessionhotkeys.moc"
